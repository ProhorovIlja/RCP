#include "nax5/nax5streamreplay.h"

#include "streamsession.h"
#include "rcp/mouse_look.h"
#include "rcp/replay_schedule.h"

#include <chiaki/audio.h>
#include <chiaki/ffmpegdecoder.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/session.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QSaveFile>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <limits>

namespace {
struct Media
{
    QList<QByteArray> video;
    QList<QByteArray> audio;
    ChiakiCodec codec = CHIAKI_CODEC_H264;
    int width = 1280, height = 720, fps = 60;
};

struct Counters
{
    std::atomic<qint64> video_sent{0}, video_dropped{0}, video_rejected{0}, video_corrupted{0};
    std::atomic<qint64> decoded_frames{0}, audio_sent{0}, audio_dropped{0}, mic_frames{0};
    std::atomic<qint64> video_held{0}, audio_held{0}, input_held{0};
    std::atomic<qint64> input_sent{0}, input_ok{0}, input_errors{0}, input_dispatch_max_us{0};
    std::atomic<qint64> last_ready_us{0}, frame_ready_gap_max_us{0}, video_submitted{0};
};

Counters counters;
std::atomic<bool> stop_requested{false};
QThread *feeder = nullptr;
QString load_error;
using Clock = std::chrono::steady_clock;

// Only touched by the GUI thread while dispatching a scripted input event.
int expected_input_phase = -1;
rcp::MouseLook scripted_mouse;
rcp::Stick scripted_stick;

void updateMax(std::atomic<qint64> &value, qint64 sample)
{
    auto old = value.load(std::memory_order_relaxed);
    while (old < sample && !value.compare_exchange_weak(old, sample, std::memory_order_relaxed)) {}
}

void queueInput(StreamSession *session, int slot)
{
    const auto posted = Clock::now();
    ++counters.input_sent;
    QMetaObject::invokeMethod(session, [session, slot, posted] {
        updateMax(counters.input_dispatch_max_us,
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - posted).count());
        const int phase = slot % 6;
        expected_input_phase = phase;
        if (phase == 1 || phase == 4)
        {
            const auto time_us = std::int64_t(slot) * 25000;
            const rcp::MouseResponse response{};
            const auto next = phase == 1 ? scripted_mouse.move(12, -5, time_us, response)
                                         : scripted_mouse.sample(time_us, response);
            scripted_stick = next.value_or(rcp::Stick{});
            if (!next || ((phase == 1) != (scripted_stick.x > 0 && scripted_stick.y < 0)))
                ++counters.input_errors;
            session->SetRcpMouseLook(scripted_stick.x, scripted_stick.y);
        }
        else
        {
            const auto type = phase == 0 || phase == 2 ? QEvent::KeyPress : QEvent::KeyRelease;
            const auto key = phase == 0 || phase == 5 ? Qt::Key_W : Qt::Key_Space;
            QKeyEvent event(type, key, Qt::NoModifier);
            if (!session->HandleRcpKey(&event))
                ++counters.input_errors;
        }
        if (expected_input_phase != -1)
        {
            ++counters.input_errors;
            expected_input_phase = -1;
        }
    }, Qt::QueuedConnection);
}

int envInt(const char *name, int fallback)
{
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(name, &ok);
    return ok ? value : fallback;
}

QList<QByteArray> readFramed(const QString &path)
{
    // [u32 little-endian size][payload] repeated; written by make-replay-media.py.
    QList<QByteArray> out;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return out;
    const QByteArray all = file.readAll();
    qsizetype pos = 0;
    while (pos + 4 <= all.size())
    {
        const auto *p = reinterpret_cast<const uchar *>(all.constData() + pos);
        const quint32 size = p[0] | (p[1] << 8) | (p[2] << 16) | (quint32(p[3]) << 24);
        pos += 4;
        if (size == 0 || pos + qsizetype(size) > all.size()) break;
        out.append(all.mid(pos, size));
        pos += size;
    }
    return out;
}

Media loadMedia()
{
    Media media;
    const QDir dir(qEnvironmentVariable("NAX5_REPLAY_DIR"));
    QFile meta_file(dir.filePath(QStringLiteral("meta.json")));
    if (!meta_file.open(QIODevice::ReadOnly))
    {
        load_error = QStringLiteral("meta.json missing");
        return media;
    }
    const auto meta = QJsonDocument::fromJson(meta_file.readAll()).object();
    const QString codec = meta.value("codec").toString();
    media.codec = codec == "h265_hdr" ? CHIAKI_CODEC_H265_HDR : codec == "h265" ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
    media.width = meta.value("width").toInt(1280);
    media.height = meta.value("height").toInt(720);
    media.fps = meta.value("fps").toInt(60);
    media.video = readFramed(dir.filePath(QStringLiteral("video.au")));
    media.audio = readFramed(dir.filePath(QStringLiteral("audio.opus")));
    if (media.video.isEmpty()) load_error = QStringLiteral("video.au empty");
    return media;
}

Media &media()
{
    static Media loaded = loadMedia();
    return loaded;
}

void writeResult(const QString &outcome, QJsonObject audio = {})
{
    const QString path = qEnvironmentVariable("NAX5_REPLAY_RESULT");
    if (path.isEmpty()) return;
    QJsonObject result{
        {"outcome", outcome},
        {"load_error", load_error},
        {"video_sent", double(counters.video_sent)},
        {"video_dropped", double(counters.video_dropped)},
        {"video_rejected_by_decoder", double(counters.video_rejected)},
        {"video_corrupted", double(counters.video_corrupted)},
        {"decoded_frames", double(counters.decoded_frames)},
        {"audio_sent", double(counters.audio_sent)},
        {"audio_dropped", double(counters.audio_dropped)},
        {"video_held", double(counters.video_held)},
        {"audio_held", double(counters.audio_held)},
        {"input_held", double(counters.input_held)},
        {"input_sent", double(counters.input_sent)},
        {"input_ok", double(counters.input_ok)},
        {"input_errors", double(counters.input_errors)},
        {"input_dispatch_max_us", double(counters.input_dispatch_max_us)},
        {"frame_ready_gap_max_us", double(counters.frame_ready_gap_max_us)},
        {"video_submitted", double(counters.video_submitted)},
        {"audio_output", audio},
        {"mic_frames", double(counters.mic_frames)},
        {"finished_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(QJsonDocument(result).toJson());
        file.commit();
    }
}
}

// Friend of StreamSession: calls the same entry points the chiaki session
// thread would call when packets arrive from the console.
class Nax5StreamReplay
{
public:
    static void run(StreamSession *session)
    {
        Media &m = media();
        if (m.video.isEmpty())
        {
            writeResult(QStringLiteral("no_media"));
            QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection);
            return;
        }
        const int seconds = std::clamp(envInt("NAX5_REPLAY_SECONDS", 20), 1, 300);
        const int loss_pct = std::clamp(envInt("NAX5_REPLAY_LOSS_PCT", 0), 0, 100);
        const int audio_loss_pct = std::clamp(envInt("NAX5_REPLAY_AUDIO_LOSS_PCT", 0), 0, 100);
        const int corrupt_pct = std::clamp(envInt("NAX5_REPLAY_CORRUPT_PCT", 0), 0, 100);
        const auto seed = static_cast<std::uint32_t>(envInt("NAX5_REPLAY_SEED", 2406));
        const rcp::ReplayTiming timing{
            seconds * rcp::secondNs,
            (std::max)(0, envInt("NAX5_REPLAY_BURST_EVERY_S", 0)) * rcp::secondNs,
            (std::max)(0, envInt("NAX5_REPLAY_BURST_HOLD_MS", 120)) * rcp::msNs,
            (std::max)(0, envInt("NAX5_REPLAY_STALL_MS", 0)) * rcp::msNs};
        const bool audio_header_only = envInt("NAX5_REPLAY_AUDIO_HEADER_ONLY", 0) != 0;
        const bool scripted_input = envInt("NAX5_REPLAY_INPUT", 0) != 0;

        ChiakiEvent connected = {};
        connected.type = CHIAKI_EVENT_CONNECTED;
        session->Event(&connected);

        ChiakiAudioSink sink;
        chiaki_opus_decoder_get_sink(&session->opus_decoder, &sink);
        if (!m.audio.isEmpty())
        {
            // PS5 audio: Opus, 2 channels, 48 kHz, 480-sample (10 ms) frames.
            ChiakiAudioHeader header;
            chiaki_audio_header_set(&header, 2, 16, 48000, 480);
            sink.header_cb(&header, sink.user);
        }

        ChiakiFfmpegDecoder *decoder = session->GetFfmpegDecoder();
        const int video_rate = std::clamp(m.fps, 1, 240);
        constexpr int audio_rate = 100, input_rate = 40;
        const int video_end = decoder ? seconds * video_rate : 0;
        const int audio_end = !audio_header_only && !m.audio.isEmpty() ? seconds * audio_rate : 0;
        const int input_end = scripted_input ? (seconds * input_rate / 6) * 6 : 0;
        constexpr auto never = (std::numeric_limits<std::int64_t>::max)();
        auto due = [&](int slot, int end, int rate) {
            return slot < end ? timing.due(timing.nominal(slot, rate)) : never;
        };
        QElapsedTimer clock;
        clock.start();
        int video = 0, audio = 0, input = 0, pending_lost = 0;
        while (!stop_requested)
        {
            const auto v = due(video, video_end, video_rate);
            const auto a = due(audio, audio_end, audio_rate);
            const auto k = due(input, input_end, input_rate);
            const auto next = (std::min)({v, a, k});
            if (next == never)
                break;
            const auto wait = next - clock.nsecsElapsed();
            if (wait > 0)
            {
                QThread::usleep(static_cast<unsigned long>(std::clamp<qint64>(wait / 1000, 100, 4000)));
                continue;
            }
            if (k == next)
            {
                counters.input_held += k > timing.nominal(input, input_rate);
                queueInput(session, input++);
            }
            else if (a == next)
            {
                const int slot = audio++;
                counters.audio_held += a > timing.nominal(slot, audio_rate);
                if (rcp::replayDrop(seed, 2, slot, audio_loss_pct))
                {
                    ++counters.audio_dropped;
                    continue;
                }
                QByteArray packet = m.audio[slot % m.audio.size()];
                sink.frame_cb(reinterpret_cast<uint8_t *>(packet.data()), size_t(packet.size()), sink.user);
                ++counters.audio_sent;
            }
            else
            {
                const int slot = video++;
                counters.video_held += v > timing.nominal(slot, video_rate);
                // Keep the opening IDR intact. Each loss decision is tied to
                // the original slot, never to timing on the Windows runner.
                if (slot && rcp::replayDrop(seed, 1, slot, loss_pct))
                {
                    ++pending_lost;
                    ++counters.video_dropped;
                    continue;
                }
                QByteArray au = m.video[slot % m.video.size()];
                if (slot && rcp::replayDrop(seed, 3, slot, corrupt_pct) && au.size() > 64)
                {
                    for (int j = 0; j < 16; ++j)
                    {
                        const auto value = rcp::replaySample(seed, 16 + j, slot);
                        au[32 + value % (au.size() - 32)] = char(value & 0xff);
                    }
                    ++counters.video_corrupted;
                }
                if (!chiaki_ffmpeg_decoder_video_sample_cb(reinterpret_cast<uint8_t *>(au.data()),
                        size_t(au.size()), pending_lost, false, decoder))
                    ++counters.video_rejected;
                pending_lost = 0;
                ++counters.video_sent;
            }
        }

        const auto outcome = stop_requested ? QStringLiteral("stopped") : QStringLiteral("completed");
        // Same GUI receiver as input callbacks: finalize after queued input.
        // Capture audio counters before shutdown and before the intentional
        // end-of-file would count as an audio underrun.
        QMetaObject::invokeMethod(session, [session, outcome] {
            const QJsonObject audio{
                {"mode", session->audio_out_pull_active.loadAcquire() ? "pull" : "queued"},
                {"opened", bool(session->audio_out_accepting.loadAcquire())},
                {"queue_resets", double(session->audio_output_queue_resets.loadRelaxed())},
                {"pull_underruns", double(session->audio_out_pull_starves.loadRelaxed())},
                {"pull_busy_callbacks", double(session->audio_out_pull_busy.loadRelaxed())},
                {"pull_discarded_pcm_bytes", double(session->audio_out_pull_discarded.loadRelaxed())}};
            writeResult(outcome, audio);
            ChiakiEvent quit = {};
            quit.type = CHIAKI_EVENT_QUIT;
            quit.quit.reason = CHIAKI_QUIT_REASON_STOPPED;
            session->Event(&quit);
            QTimer::singleShot(500, qApp, &QCoreApplication::quit);
        }, Qt::QueuedConnection);
    }
};

void nax5StreamReplayInputState(const ChiakiControllerState &state)
{
    const int phase = expected_input_phase;
    if (phase < 0)
        return;
    expected_input_phase = -1;
    const bool cross = (state.buttons & CHIAKI_CONTROLLER_BUTTON_CROSS) != 0;
    if (state.left_x == 0 && state.left_y == (phase < 5 ? -32767 : 0) &&
        state.right_x == scripted_stick.x && state.right_y == scripted_stick.y &&
        cross == (phase == 2))
        ++counters.input_ok;
    else
        ++counters.input_errors;
}

bool nax5StreamReplayActive()
{
    return qEnvironmentVariableIsSet("NAX5_REPLAY_DIR");
}

void nax5StreamReplayApply(StreamSessionConnectInfo &info)
{
    const Media &m = media();
    info.target = CHIAKI_TARGET_PS5_1;
    info.video_profile.codec = m.codec;
    info.video_profile.width = m.width;
    info.video_profile.height = m.height;
    info.video_profile.max_fps = m.fps;
    const QString decoder = qEnvironmentVariable("NAX5_REPLAY_DECODER");
    if (!decoder.isEmpty()) info.hw_decoder = decoder == "none" ? QString() : decoder;
    info.start_mic_unmuted = envInt("NAX5_REPLAY_MIC", 0) != 0;
    info.speech_processing_enabled = envInt("NAX5_REPLAY_ECHO", 0) != 0;
    if (qEnvironmentVariableIsSet("NAX5_REPLAY_AUDIO_OUT")) info.audio_out_device = qEnvironmentVariable("NAX5_REPLAY_AUDIO_OUT");
    if (qEnvironmentVariableIsSet("NAX5_REPLAY_AUDIO_IN")) info.audio_in_device = qEnvironmentVariable("NAX5_REPLAY_AUDIO_IN");
}

void nax5StreamReplayStart(StreamSession *session)
{
    stop_requested = false;
    QObject::connect(session, &StreamSession::FfmpegFrameAvailable, session,
        [] {
            ++counters.decoded_frames;
            const qint64 now = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now().time_since_epoch()).count();
            const auto previous = counters.last_ready_us.exchange(now, std::memory_order_relaxed);
            if (previous)
                updateMax(counters.frame_ready_gap_max_us, now - previous);
        }, Qt::DirectConnection);
    feeder = QThread::create([session] { Nax5StreamReplay::run(session); });
    feeder->start();
}

void nax5StreamReplayStop(StreamSession *)
{
    stop_requested = true;
    if (feeder && feeder->currentThread() != feeder)
        feeder->wait(5000);
}

void nax5StreamReplayMicFrame()
{
    ++counters.mic_frames;
}

void nax5StreamReplayVideoSubmitted()
{
    static const bool active = nax5StreamReplayActive();
    if (active)
        ++counters.video_submitted;
}

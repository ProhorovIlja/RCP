// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/feedbacksender.h>
#include <chiaki/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test-only boundaries: the real sender and payload formatter run with a
// fake monotonic clock, synchronous scheduler and captured outbound packets.
// No socket, authentication, encryption or PS5 receiver runs here.
#define CHECK(expr) do { \
	if(!(expr)) { \
		fprintf(stderr, "feedback replay line %d: %s\n", __LINE__, #expr); \
		exit(EXIT_FAILURE); \
	} \
} while(0)

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define RELEASE_TIME 20
#define END_TIME 1020

typedef struct {
	uint64_t at_ms;
	uint32_t buttons;
} InputStep;

typedef struct {
	ChiakiSeqNum16 seq;
	uint64_t at_ms;
	size_t size;
	uint8_t bytes[CHIAKI_FEEDBACK_HISTORY_PACKET_BUF_SIZE];
} HistoryPacket;

typedef struct {
	ChiakiFeedbackSender sender;
	ChiakiTakion takion;
	ChiakiThreadFunc worker;
	bool locked;
	const InputStep *steps;
	size_t step_count, next_step;
	uint64_t now_ms, end_ms;
	size_t state_count, history_count;
	HistoryPacket history[24];
} Replay;

static Replay replay;

uint64_t chiaki_time_now_monotonic_us(void) { return replay.now_ms * 1000; }

ChiakiErrorCode chiaki_thread_create(ChiakiThread *thread, ChiakiThreadFunc func, void *arg)
{
	CHECK(thread == &replay.sender.thread && arg == &replay.sender && !replay.worker);
	replay.worker = func;
	return CHIAKI_ERR_SUCCESS;
}

ChiakiErrorCode chiaki_thread_join(ChiakiThread *thread, void **retval)
{
	CHECK(thread == &replay.sender.thread && !retval && !replay.locked);
	return CHIAKI_ERR_SUCCESS;
}

ChiakiErrorCode chiaki_thread_set_name(ChiakiThread *thread, const char *name)
{
	CHECK(thread == &replay.sender.thread && name);
	return CHIAKI_ERR_SUCCESS;
}

void chiaki_thread_set_affinity(ChiakiThreadName name) { CHECK(name == CHIAKI_THREAD_NAME_FEEDBACK); }

ChiakiErrorCode chiaki_mutex_init(ChiakiMutex *mutex, bool recursive)
{
	CHECK(mutex == &replay.sender.state_mutex && !recursive && !replay.locked);
	return CHIAKI_ERR_SUCCESS;
}

ChiakiErrorCode chiaki_mutex_fini(ChiakiMutex *mutex)
{
	CHECK(mutex == &replay.sender.state_mutex && !replay.locked);
	return CHIAKI_ERR_SUCCESS;
}

ChiakiErrorCode chiaki_mutex_lock(ChiakiMutex *mutex)
{
	CHECK(mutex == &replay.sender.state_mutex && !replay.locked);
	replay.locked = true;
	return CHIAKI_ERR_SUCCESS;
}

ChiakiErrorCode chiaki_mutex_unlock(ChiakiMutex *mutex)
{
	CHECK(mutex == &replay.sender.state_mutex && replay.locked);
	replay.locked = false;
	return CHIAKI_ERR_SUCCESS;
}

ChiakiErrorCode chiaki_cond_init(ChiakiCond *cond)
{
	CHECK(cond == &replay.sender.state_cond);
	return CHIAKI_ERR_SUCCESS;
}

ChiakiErrorCode chiaki_cond_fini(ChiakiCond *cond) { return chiaki_cond_init(cond); }
ChiakiErrorCode chiaki_cond_signal(ChiakiCond *cond) { return chiaki_cond_init(cond); }

ChiakiErrorCode chiaki_cond_timedwait_pred(ChiakiCond *cond, ChiakiMutex *mutex,
		uint64_t timeout_ms, ChiakiCheckPred pred, void *user)
{
	CHECK(cond == &replay.sender.state_cond && mutex == &replay.sender.state_mutex && replay.locked);
	if(pred(user))
		return CHIAKI_ERR_SUCCESS;
	const uint64_t deadline = replay.now_ms + timeout_ms;
	if(replay.next_step < replay.step_count && replay.steps[replay.next_step].at_ms <= deadline)
	{
		const InputStep step = replay.steps[replay.next_step++];
		CHECK(step.at_ms >= replay.now_ms && step.at_ms < replay.end_ms);
		replay.now_ms = step.at_ms;
		ChiakiControllerState state;
		chiaki_controller_state_set_idle(&state);
		state.buttons = step.buttons;
		CHECK(chiaki_mutex_unlock(mutex) == CHIAKI_ERR_SUCCESS);
		CHECK(chiaki_feedback_sender_set_controller_state(&replay.sender, &state) == CHIAKI_ERR_SUCCESS);
		CHECK(chiaki_mutex_lock(mutex) == CHIAKI_ERR_SUCCESS);
		return pred(user) ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_TIMEOUT;
	}

	CHECK(timeout_ms > 0);
	if(deadline >= replay.end_ms)
	{
		replay.now_ms = replay.end_ms;
		replay.sender.should_stop = true;
		CHECK(pred(user));
		return CHIAKI_ERR_SUCCESS;
	}
	replay.now_ms = deadline;
	return CHIAKI_ERR_TIMEOUT;
}

void chiaki_log(ChiakiLog *log, ChiakiLogLevel level, const char *fmt, ...)
{
	(void)log;
	if(level == CHIAKI_LOG_ERROR)
	{
		fprintf(stderr, "feedback sender logged an unexpected error: %s\n", fmt);
		exit(EXIT_FAILURE);
	}
}

ChiakiErrorCode chiaki_takion_send_feedback_state(ChiakiTakion *takion, ChiakiSeqNum16 seq, ChiakiFeedbackState *state)
{
	CHECK(takion == &replay.takion && state && !replay.locked && seq == replay.state_count);
	++replay.state_count;
	return CHIAKI_ERR_SUCCESS;
}

ChiakiErrorCode chiaki_takion_send_feedback_history(ChiakiTakion *takion, ChiakiSeqNum16 seq, uint8_t event_count, uint8_t *payload, size_t size)
{
	CHECK(takion == &replay.takion && !replay.locked && replay.history_count < COUNT(replay.history));
	HistoryPacket *packet = &replay.history[replay.history_count++];
	CHECK(size && size <= sizeof(packet->bytes));
	CHECK(event_count == size / 3 && size % 3 == 0);
	packet->seq = seq;
	packet->at_ms = replay.now_ms;
	packet->size = size;
	memcpy(packet->bytes, payload, size);
	return CHIAKI_ERR_SUCCESS;
}

static void run(const InputStep *steps, size_t count, ChiakiSeqNum16 first_seq, uint64_t end_ms)
{
	memset(&replay, 0, sizeof(replay));
	replay.steps = steps;
	replay.step_count = count;
	replay.end_ms = end_ms;
	CHECK(chiaki_feedback_sender_init(&replay.sender, &replay.takion) == CHIAKI_ERR_SUCCESS);
	replay.sender.history_seq_num = first_seq;
	CHECK(replay.worker);
	CHECK(replay.worker(&replay.sender) == NULL);
	CHECK(replay.next_step == count && replay.now_ms == end_ms);
	chiaki_feedback_sender_fini(&replay.sender);
}

typedef struct {
	bool seen;
	ChiakiSeqNum16 seq;
	uint32_t buttons;
} ReferenceReceiver;

// A deliberately small model, not a reconstruction of Sony's receiver.
// With packet-level freshness, reject stale/duplicate packets. For these
// two test buttons, apply absolute history values oldest to newest.
static bool deliver(ReferenceReceiver *receiver, const HistoryPacket *packet)
{
	if(receiver->seen && !chiaki_seq_num_16_gt(packet->seq, receiver->seq))
		return false;
	CHECK(packet->size % 3 == 0);
	for(size_t offset = packet->size; offset > 0;)
	{
		offset -= 3;
		const uint8_t *event = packet->bytes + offset;
		CHECK(event[0] == 0x80 && (event[1] == 0x88 || event[1] == 0x89));
		CHECK(event[2] == 0xff || event[2] == 0);
		const uint32_t button = event[1] == 0x88 ? CHIAKI_CONTROLLER_BUTTON_CROSS : CHIAKI_CONTROLLER_BUTTON_MOON;
		if(event[2])
			receiver->buttons |= button;
		else
			receiver->buttons &= ~button;
	}
	receiver->seen = true;
	receiver->seq = packet->seq;
	return true;
}

static void test_replay_loss_reorder_and_wrap(void)
{
	static const InputStep steps[] = {
		{10, CHIAKI_CONTROLLER_BUTTON_CROSS},
		{20, CHIAKI_CONTROLLER_BUTTON_CROSS | CHIAKI_CONTROLLER_BUTTON_MOON},
		{30, CHIAKI_CONTROLLER_BUTTON_MOON},
	};
	run(steps, COUNT(steps), UINT16_MAX - 1, 40);
	CHECK(replay.history_count == 3);
	CHECK(replay.history[0].seq == UINT16_MAX - 1);
	CHECK(replay.history[1].seq == UINT16_MAX);
	CHECK(replay.history[2].seq == 0);
	ReferenceReceiver receiver = {0};
	// Lose the first send. The next packet must still carry the Cross press.
	CHECK(deliver(&receiver, &replay.history[1]));
	CHECK(receiver.buttons == (CHIAKI_CONTROLLER_BUTTON_CROSS | CHIAKI_CONTROLLER_BUTTON_MOON));
	CHECK(deliver(&receiver, &replay.history[2]));
	CHECK(receiver.buttons == CHIAKI_CONTROLLER_BUTTON_MOON);
	// Then delay/reorder the older press and repeat the already-seen release.
	CHECK(!deliver(&receiver, &replay.history[0]));
	CHECK(!deliver(&receiver, &replay.history[2]));
	CHECK(receiver.buttons == CHIAKI_CONTROLLER_BUTTON_MOON);
	puts("Feedback replay passed: loss with later input, stale/duplicate delivery, 16-bit wrap");
}

static void test_lost_final_release(void)
{
	static const InputStep steps[] = {{10, CHIAKI_CONTROLLER_BUTTON_CROSS}, {RELEASE_TIME, 0}};
	run(steps, COUNT(steps), 0, END_TIME);
	CHECK(replay.history_count >= 2 && replay.state_count > 0);
	CHECK(replay.history[0].at_ms == steps[0].at_ms);
	CHECK(replay.history[1].at_ms == RELEASE_TIME);
	ReferenceReceiver control = {0};
	CHECK(deliver(&control, &replay.history[0]) && control.buttons == CHIAKI_CONTROLLER_BUTTON_CROSS);
	CHECK(deliver(&control, &replay.history[1]) && control.buttons == 0);
	ReferenceReceiver damaged = {0};
	CHECK(deliver(&damaged, &replay.history[0]));
	// Drop the release, not the press. Deliver any subsequent history, but
	// never confuse periodic stick/gyro state packets with button history.
	for(size_t i = 2; i < replay.history_count; ++i)
		deliver(&damaged, &replay.history[i]);
	CHECK(damaged.buttons == 0);
	static const uint64_t resend_times[] = {40, 70, 120, 220};
	CHECK(replay.history_count == 2 + COUNT(resend_times));
	const HistoryPacket *release = &replay.history[1];
	for(size_t i = 0; i < COUNT(resend_times); ++i)
	{
		const HistoryPacket *resend = &replay.history[2 + i];
		CHECK(resend->at_ms == resend_times[i]);
		CHECK(resend->seq == release->seq && resend->size == release->size);
		CHECK(memcmp(resend->bytes, release->bytes, release->size) == 0);
	}
	printf("Feedback replay passed: lost final release recovered after %u ms idle"
			" (%zu subsequent history, %zu periodic state packets)\n",
			(unsigned)(END_TIME - RELEASE_TIME), replay.history_count - 2, replay.state_count);
}


static void test_simultaneous_buttons(void)
{
	static const InputStep steps[] = {
		{10, CHIAKI_CONTROLLER_BUTTON_CROSS | CHIAKI_CONTROLLER_BUTTON_MOON},
		{20, 0},
	};
	run(steps, COUNT(steps), 0, 30);
	CHECK(replay.history_count == 4);
	ReferenceReceiver rx = {0};
	for(size_t i = 0; i < replay.history_count; ++i)
	{
		CHECK(replay.history[i].seq == i);
		CHECK(deliver(&rx, &replay.history[i]));
		if(i == 1)
			CHECK(rx.buttons == (CHIAKI_CONTROLLER_BUTTON_CROSS | CHIAKI_CONTROLLER_BUTTON_MOON));
	}
	CHECK(rx.buttons == 0);
	puts("Candidate preserves two simultaneous press and release transitions in four numbered history packets");
}

int main(void)
{
	test_replay_loss_reorder_and_wrap();
	test_lost_final_release();
	test_simultaneous_buttons();
	return EXIT_SUCCESS;
}

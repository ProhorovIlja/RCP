// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace rcp {
struct Stick {
    int x = 0;
    int y = 0;
};

class MouseLook {
public:
    void reset() { *this = MouseLook{}; }

    std::optional<Stick> move(int dx, int dy, std::int64_t us, int sensitivity, bool invert_y)
    {
        if (dx == 0 && dy == 0)
            return std::nullopt;
        last_motion = us;
        x += dx;
        y += dy;
        return sample(us, sensitivity, invert_y);
    }

    std::optional<Stick> sample(std::int64_t us, int sensitivity, bool invert_y)
    {
        if ((x != 0 || y != 0) && (last_output < 0 || us - last_output >= sample_us)) {
            const auto interval = last_output < 0 ? sample_us : std::clamp(us - last_output, sample_us, idle_us);
            const double gain = 0.035 * std::clamp(sensitivity, 25, 300) / 100.0 * sample_us / interval;
            const double vx = x * gain;
            const double vy = y * gain * (invert_y ? -1 : 1);
            const double radius = std::hypot(vx, vy);
            const double strength = 0.12 + 0.88 * (std::min)(radius, 1.0);
            const double scale = strength * 32767.0 / radius;
            x = y = 0;
            last_output = us;
            output_active = true;
            return Stick{static_cast<int>(std::lround(vx * scale)), static_cast<int>(std::lround(vy * scale))};
        }
        if ((x != 0 || y != 0) && us - last_motion < idle_us)
            return std::nullopt;
        if (output_active && us - last_motion >= idle_us) {
            reset();
            return Stick{};
        }
        return std::nullopt;
    }

private:
    static constexpr std::int64_t sample_us = 4000;
    static constexpr std::int64_t idle_us = 16000;
    double x = 0;
    double y = 0;
    std::int64_t last_motion = -1;
    std::int64_t last_output = -1;
    bool output_active = false;
};

inline int movementAxis(bool negative, bool positive, bool diagonal)
{
    return (static_cast<int>(positive) - static_cast<int>(negative)) * (diagonal ? 23169 : 32767);
}

inline int renderQueueLimit(int configured, bool low_latency, bool pending, double average)
{
    // A two-frame cap preserves the render path's minimum while preventing extra buffering.
    if (low_latency)
        return 2;
    const int depth = (std::max)(configured, 2);
    return depth + (pending || average >= depth - 0.25 ? 1 : 0);
}
}

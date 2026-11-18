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

struct MouseResponse {
    int sensitivity = 100;
    int vertical_percent = 100;
    int min_output_percent = 0;
    int recenter_ms = 8;
    bool invert_y = false;
};

class MouseLook {
public:
    void reset() { *this = MouseLook{}; }

    std::optional<Stick> move(int dx, int dy, std::int64_t us, const MouseResponse &response)
    {
        if (dx == 0 && dy == 0)
            return std::nullopt;
        // Start at full gain even if the GUI has not dispatched the idle timer.
        if (output_active && !hasPendingMotion() && us >= idleDeadline(response))
            reset();
        last_motion = us;
        x += dx;
        y += dy;
        return sample(us, response);
    }

    std::optional<Stick> sample(std::int64_t us, const MouseResponse &response)
    {
        if (hasPendingMotion() && (last_output < 0 || us >= last_output + sample_us)) {
            const auto interval = last_output < 0 ? sample_us : std::clamp(us - last_output, sample_us, holdUs(response));
            const double gain = 0.035 * std::clamp(response.sensitivity, 25, 300) / 100.0 * sample_us / interval;
            const double vertical = std::clamp(response.vertical_percent, 25, 200) / 100.0;
            const double vx = x * gain;
            const double vy = y * gain * vertical * (response.invert_y ? -1 : 1);
            const double radius = std::hypot(vx, vy);
            const double floor = std::clamp(response.min_output_percent, 0, 30) / 100.0;
            const double strength = floor + (1.0 - floor) * (std::min)(radius, 1.0);
            const double scale = strength * 32767.0 / radius;
            x = y = 0;
            last_output = us;
            output_active = true;
            return Stick{static_cast<int>(std::lround(vx * scale)), static_cast<int>(std::lround(vy * scale))};
        }
        if (output_active && us >= idleDeadline(response)) {
            reset();
            return Stick{};
        }
        return std::nullopt;
    }

    std::optional<std::int64_t> nextDeadline(const MouseResponse &response) const
    {
        if (!output_active)
            return std::nullopt;
        const auto neutral_at = idleDeadline(response);
        return hasPendingMotion() ? (std::min)(last_output + sample_us, neutral_at) : neutral_at;
    }

private:
    bool hasPendingMotion() const { return x != 0 || y != 0; }
    static constexpr std::int64_t sample_us = 4000;
    static std::int64_t holdUs(const MouseResponse &response)
    {
        return std::clamp(response.recenter_ms, 4, 24) * 1000;
    }
    std::int64_t idleDeadline(const MouseResponse &response) const
    {
        // Give a late sample at least one tick before recentering.
        return (std::max)(last_motion + holdUs(response), last_output + sample_us);
    }

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
}

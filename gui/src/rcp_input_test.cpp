// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "rcp/mouse_look.h"
#include "rcp/render_policy.h"

#include <iostream>

int main()
{
    int failures = 0;
    const auto expect = [&failures](bool ok, const char *message) {
        if (!ok) {
            std::cerr << message << '\n';
            ++failures;
        }
    };
    rcp::MouseLook look;
    rcp::MouseResponse response;
    expect(!look.nextDeadline(response), "no scheduled work when idle");
    const auto first = look.move(1, 0, 0, response);
    expect(first && first->x == 1147 && first->y == 0, "one mouse count is immediate and only 3.5% stick by default");
    expect(look.nextDeadline(response) == 8000, "first movement schedules neutral, not a periodic poll");
    expect(!look.move(2, 0, 1000, response), "motion is coalesced");
    expect(look.nextDeadline(response) == 4000, "coalesced motion advances the deadline");
    expect(!look.move(2, 0, 2000, response), "no output per high-rate event");
    const auto second = look.sample(4000, response);
    expect(second && second->x == 4587, "all coalesced motion is delivered on the budget");
    expect(!look.sample(9999, response), "right stick has not yet centered");
    const auto stopped = look.sample(10000, response);
    expect(stopped && stopped->x == 0 && stopped->y == 0, "default is neutral 8 ms after last motion");
    expect(!look.nextDeadline(response) && !look.sample(25000, response), "no idle wakeups after center");

    response.vertical_percent = 50;
    response.invert_y = true;
    const auto invert = look.move(0, 4, 26000, response);
    expect(invert && invert->y == -2294, "vertical multiplier and inverse Y apply independently");
    look.reset();
    response.min_output_percent = 12;
    response.recenter_ms = 16;
    const auto compensated = look.move(1, 0, 30000, response);
    expect(compensated && compensated->x == 4941, "adjustment can reproduce old deadzone compensation");
    expect(look.nextDeadline(response) == 46000, "longer center hold can match a slower polling device");
    const auto new_gesture = look.move(1, 0, 50000, response);
    expect(new_gesture && new_gesture->x == 4941, "new gesture is immediate even after a late timer");
    look.reset();
    response = rcp::MouseResponse{};
    response.sensitivity = 300;
    const auto saturation = look.move(40000, 40000, 60000, response);
    expect(saturation && saturation->x > 0 && saturation->y > 0 &&
           std::hypot(saturation->x, saturation->y) <= 32768, "diagonal motion clamps to the stick circle");
    look.reset();
    response = rcp::MouseResponse{};
    look.move(1, 0, 61000, response);
    look.move(4, 0, 62000, response);
    const auto delayed = look.sample(75000, response);
    expect(delayed && delayed->x > 0 && look.nextDeadline(response) == 79000, "late sample gets a minimum hold before neutral");
    look.reset();
    expect(!look.nextDeadline(response) && !look.sample(100000, response), "focus reset clears residual motion and timer deadline");

    expect(rcp::movementAxis(true, true, false) == 0, "opposite movement keys cancel");
    expect(rcp::movementAxis(false, true, false) == 32767, "cardinal movement uses full magnitude");
    expect(rcp::movementAxis(true, false, true) == -23169, "diagonal movement is normalized");
    expect(rcp::useFrameMixing(false, false), "normal render mode keeps the current mixer path");
    expect(!rcp::useFrameMixing(true, false), "low latency cannot reinstate an automatic frame mixer");
    expect(!rcp::useFrameMixing(false, true), "explicitly disabled custom mixer is honored");
    expect(rcp::expeditedFrameTimerNeeded(false, -1), "new frame wakes an idle timer");
    expect(rcp::expeditedFrameTimerNeeded(true, 4), "new frame brings a refresh-paced timer forward");
    expect(!rcp::expeditedFrameTimerNeeded(true, 0), "zero-delay timer is already queued");
    expect(rcp::renderQueueLimit(2, false, false, 0.0) == 2, "default queue depth is unchanged");
    expect(rcp::renderQueueLimit(2, false, true, 0.0) == 3, "normal mode still adapts to backlog");
    expect(rcp::renderQueueLimit(5, false, false, 4.9) == 6, "normal mode honors custom queue depth");
    expect(rcp::renderQueueLimit(5, true, true, 5.0) == 2, "low latency caps queue even during backlog");
    return failures ? 1 : 0;
}

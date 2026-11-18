// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "rcp/mouse_look.h"

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
    const auto first = look.move(5, 0, 0, 100, false);
    expect(first && first->x > 0 && first->y == 0, "first motion is immediate");
    expect(!look.move(2, 0, 1000, 100, false), "motion is coalesced");
    expect(!look.move(2, 0, 2000, 100, false), "no packet per raw event");
    const auto second = look.sample(4000, 100, false);
    expect(second && second->x > 0, "coalesced motion is delivered on the budget");
    expect(!look.sample(17999, 100, false), "right stick is held below the idle threshold");
    const auto stopped = look.sample(18000, 100, false);
    expect(stopped && stopped->x == 0 && stopped->y == 0, "idle returns to center");
    expect(!look.sample(25000, 100, false), "no idle traffic after returning to center");
    const auto invert = look.move(0, 4, 26000, 100, true);
    expect(invert && invert->y < 0, "inverse Y takes effect");
    look.reset();
    const auto saturation = look.move(40000, 40000, 27000, 300, false);
    expect(saturation && saturation->x > 0 && saturation->y > 0 &&
           std::hypot(saturation->x, saturation->y) <= 32768, "diagonal motion clamps to the stick circle");
    look.reset();
    expect(!look.sample(100000, 100, false), "focus reset clears residual motion");

    expect(rcp::movementAxis(true, true, false) == 0, "opposite movement keys cancel");
    expect(rcp::movementAxis(false, true, false) == 32767, "cardinal movement uses full magnitude");
    expect(rcp::movementAxis(true, false, true) == -23169, "diagonal movement is normalized");
    expect(rcp::renderQueueLimit(2, false, false, 0.0) == 2, "default queue depth is unchanged");
    expect(rcp::renderQueueLimit(2, false, true, 0.0) == 3, "normal mode still adapts to backlog");
    expect(rcp::renderQueueLimit(5, false, false, 4.9) == 6, "normal mode honors custom queue depth");
    expect(rcp::renderQueueLimit(5, true, true, 5.0) == 2, "low latency caps queue even during backlog");
    return failures ? 1 : 0;
}

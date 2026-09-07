// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "Gradient.h"
#include "IOThread.h"
#include "Timer.h"

#include <memory>

CUBIC_TEST(gradient_interpolates_between_all_color_stops) {
    Gradient gradient;
    gradient.addColor({0.0, 0.0, 0.0});
    gradient.addColor({1.0, 0.0, 0.0});
    gradient.addColor({1.0, 1.0, 1.0});
    gradient.generate(5);

    CUBIC_REQUIRE(gradient.getRed().size() == 5);
    CUBIC_REQUIRE_NEAR(gradient.getRed()[0], 0.0, 0.0001);
    CUBIC_REQUIRE_NEAR(gradient.getRed()[1], 0.5, 0.0001);
    CUBIC_REQUIRE_NEAR(gradient.getRed()[2], 1.0, 0.0001);
    CUBIC_REQUIRE_NEAR(gradient.getGreen()[3], 0.5, 0.0001);
    CUBIC_REQUIRE_NEAR(gradient.getBlue()[4], 1.0, 0.0001);
}

CUBIC_TEST(gradient_handles_empty_single_and_out_of_range_colors) {
    Gradient gradient;
    gradient.generate(3);
    CUBIC_REQUIRE(gradient.getRed().size() == 3);
    CUBIC_REQUIRE_NEAR(gradient.getRed()[1], 0.0, 0.0001);

    gradient.addColor({-1.0, 2.0, 0.25});
    gradient.generate(3);
    CUBIC_REQUIRE_NEAR(gradient.getRed()[2], 0.0, 0.0001);
    CUBIC_REQUIRE_NEAR(gradient.getGreen()[2], 1.0, 0.0001);
    CUBIC_REQUIRE_NEAR(gradient.getBlue()[2], 0.25, 0.0001);

    gradient.clear();
    gradient.generate(0);
    CUBIC_REQUIRE(gradient.getRed().empty());
}

CUBIC_TEST(timer_supports_deterministic_locked_updates) {
    Timer timer;
    timer.start();
    timer.lockFramerate(20.0f);

    timer.update();
    CUBIC_REQUIRE(timer.locked());
    CUBIC_REQUIRE(timer.getMilliseconds() == 50);
    CUBIC_REQUIRE(timer.lastUpdateMilliseconds() == 50);

    timer.paused(true);
    timer.update();
    CUBIC_REQUIRE(timer.getMilliseconds() == 50);
    CUBIC_REQUIRE(timer.totalMilliseconds() == 100);
    CUBIC_REQUIRE(timer.paused());

    timer.paused(false);
    timer.update();
    CUBIC_REQUIRE(timer.getMilliseconds() == 100);
    CUBIC_REQUIRE(timer.getNumUpdates() == 3);
}

CUBIC_TEST(timer_can_set_and_reset_elapsed_time) {
    Timer timer;
    timer.start();
    timer.lockFramerate(10.0f);
    timer.update();
    timer.setMilliseconds(750);
    timer.update();
    CUBIC_REQUIRE(timer.getMilliseconds() == 850);
    CUBIC_REQUIRE_NEAR(timer.getSeconds(), 0.85, 0.0001);

    timer.reset();
    CUBIC_REQUIRE(!timer.locked());
    CUBIC_REQUIRE(timer.getNumUpdates() == 0);
}

CUBIC_TEST(rebuffer_reuses_only_released_buffers) {
    ReBuffer<int> buffers("test");
    std::shared_ptr<int> first = buffers.getBuffer();
    *first = 42;
    std::shared_ptr<int> second = buffers.getBuffer();
    CUBIC_REQUIRE(first != second);

    int *releasedAddress = first.get();
    first.reset();
    std::shared_ptr<int> reused = buffers.getBuffer();
    CUBIC_REQUIRE(reused.get() == releasedAddress);
    CUBIC_REQUIRE(*reused == 42);

    buffers.purge();
    CUBIC_REQUIRE(reused.use_count() == 1);
}

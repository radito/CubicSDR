// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "dsp/SampleFrameAccumulator.h"

#include <array>

CUBIC_TEST(frame_accumulator_waits_for_a_complete_frame) {
    SampleFrameAccumulator<int> accumulator(4);
    const std::array<int, 2> first = {1, 2};
    std::array<int, 4> frame = {};

    CUBIC_REQUIRE(!accumulator.append(first.data(), first.size(), frame.data()));
    CUBIC_REQUIRE(accumulator.bufferedSamples() == 2);
}

CUBIC_TEST(frame_accumulator_preserves_samples_across_chunks) {
    SampleFrameAccumulator<int> accumulator(4);
    const std::array<int, 2> first = {1, 2};
    const std::array<int, 2> second = {3, 4};
    std::array<int, 4> frame = {};

    CUBIC_REQUIRE(!accumulator.append(first.data(), first.size(), frame.data()));
    CUBIC_REQUIRE(accumulator.append(second.data(), second.size(), frame.data()));
    CUBIC_REQUIRE((frame == std::array<int, 4>{1, 2, 3, 4}));
}

CUBIC_TEST(frame_accumulator_keeps_the_newest_partial_history) {
    SampleFrameAccumulator<int> accumulator(4);
    const std::array<int, 3> first = {1, 2, 3};
    const std::array<int, 2> second = {4, 5};
    std::array<int, 4> frame = {};

    CUBIC_REQUIRE(!accumulator.append(first.data(), first.size(), frame.data()));
    CUBIC_REQUIRE(accumulator.append(second.data(), second.size(), frame.data()));
    CUBIC_REQUIRE((frame == std::array<int, 4>{2, 3, 4, 5}));
}

CUBIC_TEST(frame_accumulator_emits_a_rolling_latest_frame) {
    SampleFrameAccumulator<int> accumulator(4);
    const std::array<int, 4> first = {1, 2, 3, 4};
    const std::array<int, 2> second = {5, 6};
    std::array<int, 4> frame = {};

    CUBIC_REQUIRE(accumulator.append(first.data(), first.size(), frame.data()));
    CUBIC_REQUIRE(accumulator.append(second.data(), second.size(), frame.data()));
    CUBIC_REQUIRE((frame == std::array<int, 4>{3, 4, 5, 6}));
}

CUBIC_TEST(frame_accumulator_uses_the_newest_samples_from_large_chunks) {
    SampleFrameAccumulator<int> accumulator(4);
    const std::array<int, 6> samples = {1, 2, 3, 4, 5, 6};
    std::array<int, 4> frame = {};

    CUBIC_REQUIRE(accumulator.append(samples.data(), samples.size(), frame.data()));
    CUBIC_REQUIRE((frame == std::array<int, 4>{3, 4, 5, 6}));
}

CUBIC_TEST(frame_accumulator_reset_discards_buffered_samples) {
    SampleFrameAccumulator<int> accumulator(4);
    const std::array<int, 2> samples = {1, 2};
    std::array<int, 4> frame = {};

    CUBIC_REQUIRE(!accumulator.append(samples.data(), samples.size(), frame.data()));
    accumulator.reset(4);
    CUBIC_REQUIRE(accumulator.bufferedSamples() == 0);
    CUBIC_REQUIRE(!accumulator.append(samples.data(), samples.size(), frame.data()));
}

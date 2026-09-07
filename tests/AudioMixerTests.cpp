// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "AudioThread.h"

#include <atomic>
#include <thread>
#include <vector>

CUBIC_TEST(audio_stereo_ring_preserves_order_and_capacity) {
    AudioStereoRing ring(3);
    CUBIC_REQUIRE(ring.capacity() == 3);
    CUBIC_REQUIRE(ring.push(1.0f, -1.0f));
    CUBIC_REQUIRE(ring.push(2.0f, -2.0f));
    CUBIC_REQUIRE(ring.push(3.0f, -3.0f));
    CUBIC_REQUIRE(!ring.push(4.0f, -4.0f));
    CUBIC_REQUIRE(ring.available() == 3);

    float left = 0.0f;
    float right = 0.0f;
    CUBIC_REQUIRE(ring.pop(left, right));
    CUBIC_REQUIRE(left == 1.0f && right == -1.0f);
    CUBIC_REQUIRE(ring.pop(left, right));
    CUBIC_REQUIRE(left == 2.0f && right == -2.0f);
    ring.clear();
    CUBIC_REQUIRE(ring.available() == 0);
    CUBIC_REQUIRE(!ring.pop(left, right));
}

CUBIC_TEST(audio_stereo_ring_is_safe_for_one_producer_and_one_consumer) {
    constexpr int frameCount = 20000;
    AudioStereoRing ring(257);
    std::atomic_bool producerDone{false};
    std::atomic_bool orderCorrect{true};

    std::thread producer([&] {
        for (int i = 0; i < frameCount; ++i) {
            while (!ring.push(static_cast<float>(i), static_cast<float>(-i))) {
                std::this_thread::yield();
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    int expected = 0;
    while (!producerDone.load(std::memory_order_acquire) || ring.available() != 0) {
        float left = 0.0f;
        float right = 0.0f;
        if (!ring.pop(left, right)) {
            std::this_thread::yield();
            continue;
        }
        if (left != static_cast<float>(expected) || right != static_cast<float>(-expected)) {
            orderCorrect.store(false);
        }
        ++expected;
    }
    producer.join();
    CUBIC_REQUIRE(orderCorrect.load());
    CUBIC_REQUIRE(expected == frameCount);
}

CUBIC_TEST(audio_mix_state_converts_mono_and_resets_on_discontinuity) {
    AudioMixState state;
    state.sampleRate.store(48000);

    AudioThreadInput input;
    input.sampleRate = 48000;
    input.channels = 1;
    input.data = {1.0f, 2.0f, 3.0f, 4.0f};
    state.queue(input);
    CUBIC_REQUIRE(state.ring.available() == 4);

    input.discontinuity = true;
    input.data = {9.0f, 10.0f};
    state.queue(input);
    // A gap resets interpolation history but must not repeatedly throw away
    // already-buffered audio and starve CoreAudio.
    CUBIC_REQUIRE(state.ring.available() == 6);

    float left = 0.0f;
    float right = 0.0f;
    for (float expected : std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f, 9.0f, 10.0f}) {
        CUBIC_REQUIRE(state.ring.pop(left, right));
        CUBIC_REQUIRE(left == expected && right == expected);
    }

    state.clear();
    input.discontinuity = false;
    input.sampleRate = 44100;
    input.data.resize(441);
    for (size_t i = 0; i < input.data.size(); ++i) {
        input.data[i] = static_cast<float>(i);
    }
    state.queue(input);
    // Device-rate transitions are converted instead of silently discarding
    // every packet. 441 input frames at 44.1 kHz are about 480 at 48 kHz.
    CUBIC_REQUIRE(state.ring.available() >= 475);
    CUBIC_REQUIRE(state.ring.available() <= 485);

    state.clear();
    state.sampleRate.store(0);
    state.queue(input);
    CUBIC_REQUIRE(state.ring.available() == 0);
}

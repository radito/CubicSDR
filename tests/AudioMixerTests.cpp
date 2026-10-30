// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "AudioThread.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace {

// An SDR delivering 800-frame audio packets on its own clock, which runs
// `producerPpm` fast or slow against a sound card pulling 480-frame callbacks.
class PlayoutSim {
public:
    static constexpr int kRate = 48000;
    static constexpr size_t kCallback = 480;
    static constexpr size_t kPacket = 800;

    explicit PlayoutSim(double producerPpm, bool ramp = false)
        : ppm(producerPpm), rampContent(ramp) {
        state.sampleRate = kRate;
    }

    // One sound card callback; packets due by then are queued, or held back
    // as a backlog while the pipeline is stalled.
    void step(bool deliver = true) {
        cardTime += kCallback;
        while (nextPacket <= cardTime) {
            AudioThreadInput packet;
            packet.sampleRate = kRate;
            packet.channels = 1;
            for (size_t i = 0; i < kPacket; ++i, ++produced) {
                const float value = rampContent
                    ? static_cast<float>(produced % 4096) / 4096.0f + 0.001f
                    : 0.5f * static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * produced / kRate));
                packet.data.push_back(value);
                input.push_back(value);
            }
            if (deliver) {
                state.queue(packet);
            } else {
                backlog.push_back(packet);
            }
            nextPacket += kPacket / (1.0 + ppm * 1.0e-6);
        }
        bufferedFrames = state.ring.producerAvailable();
        std::vector<float> out(kCallback * 2, 0.0f);
        state.render(out.data(), kCallback);
        for (size_t i = 0; i < kCallback; ++i) played.push_back(out[i * 2]);
    }

    void run(double seconds, bool deliver = true) {
        for (int i = 0; i < seconds * kRate / kCallback; ++i) step(deliver);
    }

    void releaseBacklog() {
        for (const auto& packet : backlog) state.queue(packet);
        backlog.clear();
    }

    double bufferedMs() const { return 1000.0 * bufferedFrames / kRate; }

    AudioMixState state;
    std::vector<float> input;
    std::vector<float> played;

private:
    double ppm;
    bool rampContent;
    double cardTime = 0.0;
    double nextPacket = 0.0;
    size_t produced = 0;
    size_t bufferedFrames = 0;
    std::vector<AudioThreadInput> backlog;
};

} // namespace

CUBIC_TEST(audio_ring_flush_is_applied_by_pop_and_reports_logical_fill) {
    AudioStereoRing ring(2);
    CUBIC_REQUIRE(ring.push(1, 2));
    ring.clear();
    CUBIC_REQUIRE(ring.producerAvailable() == 0);
    CUBIC_REQUIRE(ring.push(3, 4));
    float l, r;
    CUBIC_REQUIRE(ring.pop(l, r));
    CUBIC_REQUIRE(l == 3 && r == 4);
    CUBIC_REQUIRE(ring.producerAvailable() == 0);
    CUBIC_REQUIRE(!ring.pop(l, r));
}

CUBIC_TEST(audio_mixer_rejects_invalid_packets_and_preserves_stereo) {
    AudioMixState state;
    state.sampleRate = 48000;
    AudioThreadInput packet;
    packet.sampleRate = 48000;
    packet.channels = 3;
    packet.data = {1, 2, 3, 4};
    state.queue(packet);
    CUBIC_REQUIRE(state.ring.available() == 0);
    packet.channels = 2;
    packet.sampleRate = 0;
    state.queue(packet);
    CUBIC_REQUIRE(state.ring.available() == 0);
    packet.sampleRate = 48000;
    packet.data.clear();
    state.queue(packet);
    CUBIC_REQUIRE(state.ring.available() == 0);
    packet.data = {1, -1, 2, -2, 99};
    state.queue(packet);
    CUBIC_REQUIRE(state.ring.available() == 2);
    float l, r;
    for (float expected : {1.0f, 2.0f}) {
        CUBIC_REQUIRE(state.ring.pop(l, r));
        CUBIC_REQUIRE(l == expected && r == -expected);
    }
}

CUBIC_TEST(audio_mixer_rate_conversion_is_independent_of_packet_boundaries) {
    for (int channels : {1, 2}) for (int inputRate : {24000, 96000}) {
        AudioMixState whole, chunks;
        whole.sampleRate = chunks.sampleRate = 48000;
        AudioThreadInput packet;
        packet.channels = channels;
        packet.sampleRate = inputRate;
        for (int i = 0; i < 600; ++i) {
            packet.data.push_back(i / 600.0f);
            if (channels == 2) packet.data.push_back(-i / 600.0f);
        }
        whole.queue(packet);
        const auto data = packet.data;
        for (size_t start = 0; start < data.size(); start += 17 * channels) {
            packet.data.assign(data.begin() + start, data.begin() + std::min(data.size(), start + 17 * channels));
            chunks.queue(packet);
        }
        CUBIC_REQUIRE(whole.ring.available() == chunks.ring.available());
        CUBIC_REQUIRE(whole.ring.available() > 250);
        float wl, wr, cl, cr;
        while (whole.ring.pop(wl, wr)) {
            CUBIC_REQUIRE(chunks.ring.pop(cl, cr));
            CUBIC_REQUIRE_NEAR(wl, cl, 1e-6);
            CUBIC_REQUIRE_NEAR(wr, cr, 1e-6);
            CUBIC_REQUIRE_NEAR(wr, channels == 2 ? -wl : wl, 1e-6);
        }
    }
}

CUBIC_TEST(audio_mixer_overflow_keeps_buffered_audio_playable) {
    for (int rate : {24000, 48000}) {
        AudioMixState state;
        state.sampleRate = 48000;
        state.primed = true;
        AudioThreadInput packet;
        packet.channels = 1;
        packet.sampleRate = rate;
        packet.data.assign(state.ring.capacity() + 2, 0.25f);
        state.queue(packet);
        CUBIC_REQUIRE(state.overflows == 1);
        CUBIC_REQUIRE(state.primed);
        CUBIC_REQUIRE(state.ring.available() == state.ring.capacity());
        state.clear();
        CUBIC_REQUIRE(!state.primed);
        CUBIC_REQUIRE(state.ring.available() == 0);
    }
}

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

CUBIC_TEST(audio_playout_is_bit_exact_without_clock_drift) {
    PlayoutSim sim(0.0, true);
    sim.run(60.0);
    CUBIC_REQUIRE(sim.state.underflows == 0);
    CUBIC_REQUIRE(sim.state.rateTrimPpm == 0.0f);
    const auto start = std::find_if(sim.played.begin(), sim.played.end(),
                                    [](float v) { return v != 0.0f; });
    CUBIC_REQUIRE(start != sim.played.end());
    const size_t offset = static_cast<size_t>(start - sim.played.begin());
    for (size_t i = offset; i < sim.played.size(); ++i) {
        CUBIC_REQUIRE(sim.played[i] == sim.input[i - offset]);
    }
}

CUBIC_TEST(audio_playout_holds_latency_through_clock_drift) {
    // Uncontrolled, 300 ppm over five minutes is 90 ms of drift: latency
    // creeping up, or the ring running dry every few minutes.
    for (double ppm : {300.0, -300.0}) {
        PlayoutSim sim(ppm);
        sim.run(10.0);
        double worstMs = 0.0;
        for (int second = 0; second < 290; ++second) {
            sim.run(1.0);
            worstMs = std::max(worstMs, sim.bufferedMs());
        }
        CUBIC_REQUIRE(sim.state.underflows == 0);
        CUBIC_REQUIRE(sim.state.overflows == 0);
        CUBIC_REQUIRE(worstMs < 60.0);
        CUBIC_REQUIRE(std::fabs(sim.state.rateTrimPpm.load()) <= 500.0f);

        // No clicks: a 1 kHz tone at 0.5 never steps more than 0.066.
        const auto start = std::find_if(sim.played.begin(), sim.played.end(),
                                        [](float v) { return v != 0.0f; });
        float largestStep = 0.0f;
        for (auto it = start + 1; it < sim.played.end(); ++it) {
            largestStep = std::max(largestStep, std::fabs(*it - *(it - 1)));
        }
        CUBIC_REQUIRE(largestStep < 0.07f);
    }
}

CUBIC_TEST(audio_playout_skips_a_stall_backlog) {
    PlayoutSim sim(0.0);
    sim.run(5.0);
    const double normalMs = sim.bufferedMs();
    // The demodulator stalls for 300 ms while the SDR keeps sampling; the
    // ring runs dry, then the whole backlog arrives at once.
    sim.run(0.3, false);
    CUBIC_REQUIRE(sim.state.underflows == 1);
    sim.releaseBacklog();
    sim.run(1.0);
    CUBIC_REQUIRE(sim.state.catchUps == 1);
    CUBIC_REQUIRE(sim.bufferedMs() < normalMs + 25.0);
}

CUBIC_TEST(audio_playout_counts_only_mid_stream_gaps_as_dropouts) {
    PlayoutSim sim(0.0);
    sim.run(2.0);
    sim.run(0.05, false);  // brief stall: audio stops and resumes
    sim.releaseBacklog();
    sim.run(1.0);
    CUBIC_REQUIRE(sim.state.dropouts == 1);
    sim.run(2.0, false);  // transmission ends, e.g. squelch closes
    sim.run(1.0);
    CUBIC_REQUIRE(sim.state.underflows == 2);
    CUBIC_REQUIRE(sim.state.dropouts == 1);
}

CUBIC_TEST(audio_playout_keeps_squelch_preroll) {
    // A squelch opening queues 50 ms of pre-roll with its first block; that is
    // audio worth hearing, not a backlog to skip.
    AudioMixState state;
    state.sampleRate = 48000;
    AudioThreadInput packet;
    packet.sampleRate = 48000;
    packet.channels = 1;
    packet.data.assign(3200, 0.25f);
    state.queue(packet);
    size_t heard = 0;
    for (int callback = 0; callback < 20; ++callback) {
        std::vector<float> out(960, 0.0f);
        state.render(out.data(), 480);
        for (size_t i = 0; i < 480; ++i) heard += out[i * 2] == 0.25f;
    }
    CUBIC_REQUIRE(state.catchUps == 0);
    // All but the interpolation lookahead held when the stream ends.
    CUBIC_REQUIRE(heard >= 3200 - 9);
}

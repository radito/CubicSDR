// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "SquelchProcessor.h"

#include "liquid/liquid.h"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace {

struct BlockSignal {
    bool carrier = true;
    float snrDb = 30.0f;
    bool voice = true;
    float offsetHz = 0.0f;
    float ctcssDeviationHz = 0.0f;
};

// Generates NBFM IQ in 10 ms blocks over a -20 dB noise floor, demodulates it
// like ModemNBFM (freqdem kf = 0.5) and applies DemodulatorThread's squelch
// sequence, so tests see exactly when the squelch would open and close.
class SquelchRig {
public:
    explicit SquelchRig(float closeLevelDb, int sampleRate = 12500)
        : closeLevel(closeLevelDb), rate(sampleRate), blockSize(sampleRate / 100),
          demod(freqdem_create(0.5f)), rng(5) {}
    ~SquelchRig() { freqdem_destroy(demod); }
    SquelchRig(const SquelchRig&) = delete;
    SquelchRig& operator=(const SquelchRig&) = delete;

    bool feed(const BlockSignal& signal) {
        constexpr double twoPi = 2.0 * 3.14159265358979323846;
        constexpr double noiseFloor = 0.01;
        const float noiseSigma = static_cast<float>(std::sqrt(noiseFloor / 2.0));
        const double amplitude = signal.carrier
            ? std::sqrt(noiseFloor * std::pow(10.0, signal.snrDb / 10.0)) : 0.0;
        std::normal_distribution<float> gaussian(0.0f, noiseSigma);
        std::vector<liquid_float_complex> iq(blockSize);
        std::vector<float> discriminator(blockSize);
        double power = 0.0;
        for (size_t i = 0; i < blockSize; ++i, ++sample) {
            const double t = double(sample) / rate;
            // Two-tone "speech" with a syllable envelope.
            const double voice = signal.voice
                ? 2500.0 * (0.6 * std::sin(twoPi * 450.0 * t) + 0.4 * std::sin(twoPi * 1300.0 * t)) *
                  (0.5 + 0.5 * std::sin(twoPi * 3.0 * t))
                : 0.0;
            phase += twoPi * (voice + signal.ctcssDeviationHz * std::sin(twoPi * 100.0 * t) +
                              signal.offsetHz) / rate;
            iq[i].real = float(amplitude * std::cos(phase)) + gaussian(rng);
            iq[i].imag = float(amplitude * std::sin(phase)) + gaussian(rng);
            power += double(iq[i].real) * iq[i].real + double(iq[i].imag) * iq[i].imag;
        }
        freqdem_demodulate_block(demod, iq.data(), static_cast<unsigned int>(blockSize),
                                 discriminator.data());

        const float level = detector.update(
            static_cast<float>(10.0 * std::log10(power / blockSize)), 0.01);
        hasQuality = useQuality && noise.process(discriminator, rate, false, quality);
        if (!open && SquelchDetector::shouldOpen(level, closeLevel, hasQuality, quality)) {
            open = true;
            lastHeld = now;
        }
        if (open) {
            const std::int64_t hang = SquelchDetector::closeHangMilliseconds(
                level, closeLevel, hasQuality, quality);
            if (hang == 0) {
                lastHeld = now;
            } else if (now - lastHeld > hang) {
                open = false;
            }
        }
        now += 10;
        return open;
    }

    // Feeds `milliseconds` of one signal; returns how many 10 ms blocks were open.
    int run(int milliseconds, const BlockSignal& signal) {
        int openBlocks = 0;
        for (int elapsed = 0; elapsed < milliseconds; elapsed += 10) openBlocks += feed(signal);
        return openBlocks;
    }

    // Milliseconds until the squelch closes, or -1 if it stays open.
    int millisecondsToClose(int limit, const BlockSignal& signal) {
        for (int elapsed = 10; elapsed <= limit; elapsed += 10) {
            if (!feed(signal)) return elapsed;
        }
        return -1;
    }

    bool useQuality = true;
    bool open = false;
    bool hasQuality = false;
    float quality = 0.0f;

private:
    float closeLevel;
    int rate;
    size_t blockSize;
    freqdem demod;
    std::mt19937 rng;
    SquelchDetector detector;
    NBFMNoiseDetector noise;
    double phase = 0.0;
    std::uint64_t sample = 0;
    std::int64_t now = 0;
    std::int64_t lastHeld = 0;
};

BlockSignal speech(float snrDb) { return {true, snrDb, true}; }
BlockSignal pause(float snrDb) { return {true, snrDb, false}; }
BlockSignal noCarrier() { return {false, 0.0f, false}; }

} // namespace

CUBIC_TEST(squelch_detector_uses_fast_attack_and_slow_release) {
    SquelchDetector detector;
    CUBIC_REQUIRE_NEAR(detector.update(-80.0f, 0.01), -80.0f, 0.001f);

    const float attacked = detector.update(-40.0f, 0.02);
    CUBIC_REQUIRE(attacked > -60.0f);
    CUBIC_REQUIRE(attacked < -40.0f);

    const float released = detector.update(-80.0f, 0.02);
    CUBIC_REQUIRE(released < attacked);
    CUBIC_REQUIRE(released > -60.0f);
    CUBIC_REQUIRE_NEAR(detector.update(-55.0f, 0.01, true), -55.0f, 0.001f);
}

CUBIC_TEST(squelch_hang_depends_on_why_the_signal_failed) {
    // Held while level and quality are good.
    CUBIC_REQUIRE(SquelchDetector::closeHangMilliseconds(-40.0f, -50.0f, true, 1.0f) == 0);
    // A level dip on clean audio is fading: hang long.
    CUBIC_REQUIRE(SquelchDetector::closeHangMilliseconds(-55.0f, -50.0f, true, 1.0f) == 500);
    CUBIC_REQUIRE(SquelchDetector::closeHangMilliseconds(-55.0f, -50.0f, false, 0.0f) == 500);
    // Noise means the carrier is gone: close quickly to cut the tail.
    CUBIC_REQUIRE(SquelchDetector::closeHangMilliseconds(-40.0f, -50.0f, true, 0.0f) == 60);
    CUBIC_REQUIRE(SquelchDetector::shouldOpen(-47.0f, -50.0f, false, 0.0f));
    CUBIC_REQUIRE(!SquelchDetector::shouldOpen(-49.0f, -50.0f, false, 0.0f));
    CUBIC_REQUIRE(!SquelchDetector::shouldOpen(-40.0f, -50.0f, true, 0.1f));
}

CUBIC_TEST(nbfm_noise_detector_distinguishes_voice_band_audio_from_fm_noise) {
    constexpr int sampleRate = 12500;
    std::vector<float> tone(4096);
    std::vector<float> noise(4096);
    std::uint32_t randomState = 1;
    for (std::size_t i = 0; i < tone.size(); ++i) {
        tone[i] = 0.25f * std::sin(2.0 * 3.14159265358979323846 * 1000.0 *
                                  static_cast<double>(i) / sampleRate);
        randomState = randomState * 1664525u + 1013904223u;
        noise[i] = static_cast<float>((randomState >> 8) / 8388607.5 - 1.0);
    }

    NBFMNoiseDetector cleanDetector;
    NBFMNoiseDetector noisyDetector;
    float cleanQuality = 0.0f;
    float noisyQuality = 1.0f;
    CUBIC_REQUIRE(cleanDetector.process(tone, sampleRate, true, cleanQuality));
    CUBIC_REQUIRE(noisyDetector.process(noise, sampleRate, true, noisyQuality));
    CUBIC_REQUIRE(cleanQuality > 0.70f);
    CUBIC_REQUIRE(noisyQuality < 0.35f);
    CUBIC_REQUIRE(cleanQuality > noisyQuality + 0.50f);
}

CUBIC_TEST(nbfm_noise_detector_rejects_rates_without_a_noise_band) {
    NBFMNoiseDetector detector;
    float quality = 0.0f;
    CUBIC_REQUIRE(!detector.process(std::vector<float>(100, 0.0f), 6000, true, quality));
    CUBIC_REQUIRE(detector.process(std::vector<float>(100, 0.0f), 7000, true, quality));
}

CUBIC_TEST(nbfm_quality_ignores_modulation_tuning_offset_and_ctcss) {
    // The old energy-ratio metric scored a quiet carrier as pure noise unless
    // it happened to be off-tune or carry CTCSS.
    for (int rate : {8000, 12500, 25000}) {
        const BlockSignal clean[] = {
            speech(15.0f), pause(15.0f), {true, 15.0f, false, 600.0f, 0.0f},
            {true, 15.0f, false, 0.0f, 500.0f}};
        for (const BlockSignal& signal : clean) {
            SquelchRig rig(-30.0f, rate);
            rig.run(300, signal);
            CUBIC_REQUIRE(rig.hasQuality);
            CUBIC_REQUIRE(rig.quality > 0.9f);
        }
        SquelchRig openChannel(-30.0f, rate);
        openChannel.run(300, noCarrier());
        CUBIC_REQUIRE(openChannel.quality < 0.1f);
    }
}

CUBIC_TEST(squelch_stays_open_through_speech_pauses) {
    SquelchRig rig(-14.0f);
    rig.run(100, speech(30.0f));
    CUBIC_REQUIRE(rig.open);
    CUBIC_REQUIRE(rig.run(1000, pause(30.0f)) == 100);
    CUBIC_REQUIRE(rig.run(500, speech(30.0f)) == 50);
}

CUBIC_TEST(squelch_holds_weak_readable_signals) {
    SquelchRig rig(-17.0f);
    rig.run(200, speech(10.0f));
    CUBIC_REQUIRE(rig.open);
    for (int i = 0; i < 5; ++i) {
        CUBIC_REQUIRE(rig.run(300, speech(10.0f)) == 30);
        CUBIC_REQUIRE(rig.run(300, pause(10.0f)) == 30);
    }
}

CUBIC_TEST(squelch_quality_keeps_open_channel_closed_below_level_threshold) {
    // Squelch level set under the noise floor: level alone would open.
    SquelchRig rig(-25.0f);
    CUBIC_REQUIRE(rig.run(3000, noCarrier()) == 0);
    SquelchRig levelOnly(-25.0f);
    levelOnly.useQuality = false;
    CUBIC_REQUIRE(levelOnly.run(100, noCarrier()) > 0);
}

CUBIC_TEST(squelch_closes_quickly_when_the_carrier_drops) {
    // Whether or not the level threshold can see the drop, the noise burst
    // after the carrier ends is cut within about a tenth of a second.
    for (float closeLevel : {-25.0f, -14.0f}) {
        SquelchRig rig(closeLevel);
        rig.run(500, speech(30.0f));
        CUBIC_REQUIRE(rig.open);
        const int closedAfter = rig.millisecondsToClose(1000, noCarrier());
        CUBIC_REQUIRE(closedAfter > 0);
        CUBIC_REQUIRE(closedAfter <= 120);
        CUBIC_REQUIRE(rig.run(2000, noCarrier()) == 0);
    }
}

CUBIC_TEST(squelch_rides_through_a_level_fade_with_clean_audio) {
    // Squelch set well above the floor; a 300 ms fade drops the level below
    // it while the audio stays clean, as on a mobile signal.
    SquelchRig rig(-5.0f);
    rig.run(500, speech(25.0f));
    CUBIC_REQUIRE(rig.open);
    CUBIC_REQUIRE(rig.run(300, speech(12.0f)) == 30);
    CUBIC_REQUIRE(rig.run(500, speech(25.0f)) == 50);
}

CUBIC_TEST(squelch_audio_gate_preserves_preroll_and_fades_transitions) {
    SquelchAudioGate gate;
    std::vector<float> output;
    std::vector<float> firstClosed(40, 1.0f);
    std::vector<float> secondClosed(20, 2.0f);
    std::vector<float> opened(10, 3.0f);

    CUBIC_REQUIRE(gate.process(firstClosed, 1000, 1, true, false, false, output) ==
                  SquelchAudioResult::Suppress);
    CUBIC_REQUIRE(gate.process(secondClosed, 1000, 1, true, false, false, output) ==
                  SquelchAudioResult::Suppress);
    CUBIC_REQUIRE(gate.process(opened, 1000, 1, true, true, false, output) ==
                  SquelchAudioResult::Processed);

    CUBIC_REQUIRE(output.size() == 60);
    CUBIC_REQUIRE_NEAR(output.front(), 0.0f, 0.001f);
    CUBIC_REQUIRE_NEAR(output[29], 1.0f, 0.001f);
    CUBIC_REQUIRE_NEAR(output[30], 2.0f, 0.001f);
    CUBIC_REQUIRE_NEAR(output[50], 3.0f, 0.001f);

    CUBIC_REQUIRE(gate.process(opened, 1000, 1, true, true, false, output) ==
                  SquelchAudioResult::PassThrough);
    CUBIC_REQUIRE(gate.process(opened, 1000, 1, true, false, false, output) ==
                  SquelchAudioResult::Processed);
    CUBIC_REQUIRE(output.size() == 8);
    CUBIC_REQUIRE_NEAR(output.back(), 0.0f, 0.001f);
    CUBIC_REQUIRE(gate.process(opened, 1000, 1, true, false, false, output) ==
                  SquelchAudioResult::Suppress);
}

CUBIC_TEST(squelch_audio_gate_discards_preroll_across_a_stream_gap) {
    SquelchAudioGate gate;
    std::vector<float> output;
    const std::vector<float> closed(20, 1.0f);
    const std::vector<float> opened(10, 2.0f);

    gate.process(closed, 1000, 1, true, false, false, output);
    CUBIC_REQUIRE(gate.process(opened, 1000, 1, true, true, true, output) ==
                  SquelchAudioResult::Processed);
    CUBIC_REQUIRE(output.size() == opened.size());
}

CUBIC_TEST(squelch_rides_through_a_brief_mobile_dropout) {
    // Closing fast on noise must not chop short picket-fencing dropouts.
    SquelchRig rig(-14.0f);
    rig.run(500, speech(30.0f));
    rig.run(50, noCarrier());
    CUBIC_REQUIRE(rig.open);
    CUBIC_REQUIRE(rig.run(300, speech(30.0f)) == 30);
}

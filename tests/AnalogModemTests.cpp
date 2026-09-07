// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "ModemAM.h"
#include "ModemCW.h"
#include "ModemDSB.h"
#include "ModemFM.h"
#include "ModemFMStereo.h"
#include "ModemLSB.h"
#include "ModemNBFM.h"
#include "ModemUSB.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

struct KitDeleter {
    Modem *modem;
    void operator()(ModemKit *kit) const {
        if (kit) {
            modem->disposeKit(kit);
        }
    }
};

ModemIQData makeAnalogSignal(size_t count, long long sampleRate) {
    ModemIQData input;
    input.sampleRate = sampleRate;
    input.data.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const float time = static_cast<float>(i) / static_cast<float>(sampleRate);
        const float amplitude = 1.0f + 0.25f * std::sin(2.0f * static_cast<float>(M_PI) * 700.0f * time);
        const float phase = 2.0f * static_cast<float>(M_PI) * 1200.0f * time;
        input.data[i].real = amplitude * std::cos(phase);
        input.data[i].imag = amplitude * std::sin(phase);
    }
    return input;
}

void requireFiniteAudio(const AudioThreadInput& output, int channels, int sampleRate) {
    CUBIC_REQUIRE(output.channels == channels);
    CUBIC_REQUIRE(output.sampleRate == sampleRate);
    CUBIC_REQUIRE(!output.data.empty());
    CUBIC_REQUIRE(std::all_of(output.data.begin(), output.data.end(),
                              [](float sample) { return std::isfinite(sample); }));
}

template <typename ModemType>
void exerciseMonoModem(long long sampleRate) {
    ModemType modem;
    std::unique_ptr<ModemKit, KitDeleter> kit(
        modem.buildKit(sampleRate, 48'000), KitDeleter{&modem});
    ModemIQData input = makeAnalogSignal(4096, sampleRate);
    AudioThreadInput output;

    modem.demodulate(kit.get(), &input, &output);
    requireFiniteAudio(output, 1, 48'000);

    ModemIQData empty;
    modem.demodulate(kit.get(), &empty, &output);
}

} // namespace

CUBIC_TEST(am_modem_demodulates_finite_mono_audio) {
    exerciseMonoModem<ModemAM>(12'000);
}

CUBIC_TEST(am_recovers_a_clean_tone_across_small_blocks_and_a_stream_gap) {
    ModemAM modem;
    std::unique_ptr<ModemKit, KitDeleter> kit(
        modem.buildKit(12000, 48000), KitDeleter{&modem});
    // 5 ms IQ packets exercise the stateful DC blocker and audio resampler.
    // Measure recovered 1 kHz energy after settling on either side of a gap.
    for (int segment = 0; segment < 2; ++segment) {
        std::vector<float> recovered;
        for (size_t block = 0; block < 80; ++block) {
            ModemIQData input;
            input.sampleRate = 12000;
            input.discontinuity = block == 0;
            input.data.resize(60);
            for (size_t i = 0; i < input.data.size(); ++i) {
                const double phase = 2.0 * M_PI * 1000.0 * (block * 60 + i) / 12000.0;
                input.data[i].real = 1.0f + 0.4f * std::sin(phase);
                input.data[i].imag = 0.0f;
            }
            AudioThreadInput output;
            modem.demodulate(kit.get(), &input, &output);
            requireFiniteAudio(output, 1, 48000);
            if (block >= 40) recovered.insert(recovered.end(), output.data.begin(), output.data.end());
        }
        CUBIC_REQUIRE(recovered.size() >= 9000);
        double sine = 0, cosine = 0, energy = 0;
        for (size_t i = 0; i < recovered.size(); ++i) {
            const double phase = 2.0 * M_PI * 1000.0 * i / 48000.0;
            sine += recovered[i] * std::sin(phase);
            cosine += recovered[i] * std::cos(phase);
            energy += recovered[i] * recovered[i];
        }
        CUBIC_REQUIRE(energy / recovered.size() > 0.001);
        const double toneFraction = 2.0 * (sine * sine + cosine * cosine) / (recovered.size() * energy);
        CUBIC_REQUIRE(toneFraction > 0.98);
    }
}

namespace {
template <typename ModemType>
void requireSidebandTuningSurvivesReset() {
    ModemType reference, reset;
    std::unique_ptr<ModemKit, KitDeleter> referenceKit(
        reference.buildKit(12000, 48000), KitDeleter{&reference});
    std::unique_ptr<ModemKit, KitDeleter> resetKit(
        reset.buildKit(12000, 48000), KitDeleter{&reset});
    auto input = makeAnalogSignal(4096, 12000);
    AudioThreadInput expected, actual;
    reference.demodulate(referenceKit.get(), &input, &expected);
    input.discontinuity = true;
    reset.demodulate(resetKit.get(), &input, &actual);
    requireFiniteAudio(actual, 1, 48000);
    CUBIC_REQUIRE(expected.data.size() == actual.data.size());
    for (size_t i = 0; i < actual.data.size(); ++i) {
        CUBIC_REQUIRE_NEAR(actual.data[i], expected.data[i], 1e-5);
    }
}
}

CUBIC_TEST(usb_stream_reset_preserves_sideband_tuning) {
    requireSidebandTuningSurvivesReset<ModemUSB>();
}

CUBIC_TEST(lsb_stream_reset_preserves_sideband_tuning) {
    requireSidebandTuningSurvivesReset<ModemLSB>();
}

CUBIC_TEST(dsb_modem_demodulates_finite_mono_audio) {
    exerciseMonoModem<ModemDSB>(12'000);
}

CUBIC_TEST(fm_modem_demodulates_finite_mono_audio) {
    exerciseMonoModem<ModemFM>(200'000);
}

CUBIC_TEST(fm_demod_audio_reaches_the_playback_ring_after_a_stream_reset) {
    ModemFM modem;
    std::unique_ptr<ModemKit, KitDeleter> kit(
        modem.buildKit(200'000, 48'000), KitDeleter{&modem});
    ModemIQData input = makeAnalogSignal(4096, 200'000);
    input.discontinuity = true;
    AudioThreadInput output;
    modem.demodulate(kit.get(), &input, &output);
    output.discontinuity = input.discontinuity;

    AudioMixState playback;
    playback.sampleRate.store(48'000);
    playback.queue(output);
    CUBIC_REQUIRE(playback.ring.available() > 0);

    float peak = 0.0f;
    float left = 0.0f;
    float right = 0.0f;
    while (playback.ring.pop(left, right)) {
        CUBIC_REQUIRE(std::isfinite(left));
        CUBIC_REQUIRE(left == right);
        peak = std::max(peak, std::fabs(left));
    }
    CUBIC_REQUIRE(peak > 0.0f);
}

CUBIC_TEST(nbfm_modem_demodulates_finite_mono_audio) {
    exerciseMonoModem<ModemNBFM>(12'500);
}

CUBIC_TEST(lsb_modem_demodulates_finite_mono_audio) {
    exerciseMonoModem<ModemLSB>(12'000);
}

CUBIC_TEST(usb_modem_demodulates_finite_mono_audio) {
    exerciseMonoModem<ModemUSB>(12'000);
}

CUBIC_TEST(cw_modem_demodulates_and_round_trips_settings) {
    ModemCW modem;
    CUBIC_REQUIRE(modem.readSetting("offset") == "650.000000");
    CUBIC_REQUIRE(modem.readSetting("auto") == "on");
    modem.writeSetting("offset", "750");
    modem.writeSetting("auto", "off");
    modem.writeSetting("gain", "6");
    CUBIC_REQUIRE(modem.shouldRebuildKit());
    CUBIC_REQUIRE(modem.readSetting("offset") == "750.000000");
    CUBIC_REQUIRE(modem.readSetting("auto") == "off");
    CUBIC_REQUIRE(modem.getSettings().size() == 3);

    std::unique_ptr<ModemKit, KitDeleter> kit(
        modem.buildKit(6'000, 48'000), KitDeleter{&modem});
    ModemIQData input = makeAnalogSignal(512, 6'000);
    AudioThreadInput output;
    modem.demodulate(kit.get(), &input, &output);
    requireFiniteAudio(output, 1, 48'000);
}

CUBIC_TEST(fm_stereo_modem_demodulates_and_owns_its_kit) {
    ModemFMStereo modem;
    CUBIC_REQUIRE(modem.readSetting("demph") == "75");
    modem.writeSetting("demph", "50");
    CUBIC_REQUIRE(modem.shouldRebuildKit());
    CUBIC_REQUIRE(modem.getSettings().size() == 1);

    std::unique_ptr<ModemKit, KitDeleter> kit(
        modem.buildKit(200'000, 48'000), KitDeleter{&modem});
    ModemIQData input = makeAnalogSignal(8192, 200'000);
    AudioThreadInput output;
    modem.demodulate(kit.get(), &input, &output);
    requireFiniteAudio(output, 2, 48'000);
    CUBIC_REQUIRE(output.data.size() % 2 == 0);

    ModemIQData empty;
    modem.demodulate(kit.get(), &empty, &output);
}

CUBIC_TEST(analog_modems_normalize_sample_rates_and_identify_themselves) {
    ModemAM am;
    ModemFM fm;
    ModemNBFM nbfm;
    ModemLSB lsb;
    ModemUSB usb;
    ModemFMStereo stereo;

    CUBIC_REQUIRE(am.getName() == "AM");
    CUBIC_REQUIRE(fm.getName() == "FM");
    CUBIC_REQUIRE(nbfm.getName() == "NBFM");
    CUBIC_REQUIRE(lsb.getName() == "LSB");
    CUBIC_REQUIRE(usb.getName() == "USB");
    CUBIC_REQUIRE(stereo.getName() == "FMS");
    CUBIC_REQUIRE(am.checkSampleRate(100, 48'000) == MIN_BANDWIDTH);
    CUBIC_REQUIRE(lsb.checkSampleRate(5'401, 48'000) == 5'402);
    CUBIC_REQUIRE(usb.checkSampleRate(5'401, 48'000) == 5'402);
    CUBIC_REQUIRE(stereo.checkSampleRate(50'000, 48'000) == 120'000);
}

CUBIC_TEST(fm_and_nbfm_expose_deemphasis_and_reset_cleanly_after_a_gap) {
    ModemFM fm;
    ModemNBFM nbfm;
    CUBIC_REQUIRE(fm.readSetting("demph") == "75");
    CUBIC_REQUIRE(nbfm.readSetting("demph") == "0");
    fm.writeSetting("demph", "50");
    nbfm.writeSetting("demph", "300");
    CUBIC_REQUIRE(fm.readSetting("demph") == "50");
    CUBIC_REQUIRE(nbfm.readSetting("demph") == "300");
    CUBIC_REQUIRE(fm.getSettings().size() == 1);
    CUBIC_REQUIRE(nbfm.getSettings().size() == 1);

    std::unique_ptr<ModemKit, KitDeleter> fmKit(
        fm.buildKit(200'000, 48'000), KitDeleter{&fm});
    ModemIQData input = makeAnalogSignal(4096, 200'000);
    input.discontinuity = true;
    AudioThreadInput output;
    fm.demodulate(fmKit.get(), &input, &output);
    requireFiniteAudio(output, 1, 48'000);
}

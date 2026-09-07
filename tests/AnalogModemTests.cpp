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

CUBIC_TEST(dsb_modem_demodulates_finite_mono_audio) {
    exerciseMonoModem<ModemDSB>(12'000);
}

CUBIC_TEST(fm_modem_demodulates_finite_mono_audio) {
    exerciseMonoModem<ModemFM>(200'000);
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
    CUBIC_REQUIRE(stereo.checkSampleRate(50'000, 48'000) == 100'000);
}

// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "ModemAPSK.h"
#include "ModemASK.h"
#include "ModemBPSK.h"
#include "ModemDPSK.h"
#include "ModemFSK.h"
#include "ModemGMSK.h"
#include "ModemOOK.h"
#include "ModemPSK.h"
#include "ModemQAM.h"
#include "ModemQPSK.h"
#include "ModemSQAM.h"
#include "ModemST.h"

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct KitDeleter {
    Modem *modem;
    void operator()(ModemKit *kit) const {
        if (kit) {
            modem->disposeKit(kit);
        }
    }
};

ModemIQData makeDigitalSignal(size_t count, long long sampleRate = 19'200) {
    ModemIQData input;
    input.sampleRate = sampleRate;
    input.data.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const float phase = static_cast<float>(i % 4) * static_cast<float>(M_PI) / 2.0f;
        input.data[i].real = std::cos(phase);
        input.data[i].imag = std::sin(phase);
    }
    return input;
}

template <typename ModemType>
void exerciseSimpleDigitalModem(const std::string& expectedName) {
    ModemType modem;
    CUBIC_REQUIRE(modem.getName() == expectedName);
    CUBIC_REQUIRE(modem.getType() == "digital");
    CUBIC_REQUIRE(!modem.getDemodulatorLock());

    std::unique_ptr<ModemKit, KitDeleter> kit(
        modem.buildKit(19'200, 48'000), KitDeleter{&modem});
    ModemIQData input = makeDigitalSignal(64);
    AudioThreadInput output;
    modem.demodulate(kit.get(), &input, &output);
    CUBIC_REQUIRE(modem.getDemodulatorLock() == 0 || modem.getDemodulatorLock() == 1);
}

template <typename ModemType>
void exerciseConstellationSetting(const std::string& initial, const std::string& changed) {
    ModemType modem;
    CUBIC_REQUIRE(modem.getSettings().size() == 3);
    CUBIC_REQUIRE(modem.readSetting("cons") == initial);
    modem.writeSetting("cons", changed);
    modem.writeSetting("sps", "4");
    modem.writeSetting("beta", "0.25");
    CUBIC_REQUIRE(modem.readSetting("cons") == changed);
    CUBIC_REQUIRE(modem.readSetting("sps") == "4");
    CUBIC_REQUIRE(std::stof(modem.readSetting("beta")) == 0.25f);
}

} // namespace

CUBIC_TEST(fixed_constellation_digital_modems_execute) {
    exerciseSimpleDigitalModem<ModemBPSK>("BPSK");
    exerciseSimpleDigitalModem<ModemQPSK>("QPSK");
    exerciseSimpleDigitalModem<ModemOOK>("OOK");
    exerciseSimpleDigitalModem<ModemST>("ST");
}

CUBIC_TEST(configurable_digital_constellations_round_trip) {
    exerciseConstellationSetting<ModemASK>("2", "16");
    exerciseConstellationSetting<ModemAPSK>("4", "32");
    exerciseConstellationSetting<ModemDPSK>("2", "8");
    exerciseConstellationSetting<ModemPSK>("2", "64");
    exerciseConstellationSetting<ModemQAM>("4", "16");
    exerciseConstellationSetting<ModemSQAM>("32", "128");
}

CUBIC_TEST(configurable_digital_modems_execute_selected_constellations) {
    ModemASK ask;
    ModemAPSK apsk;
    ModemDPSK dpsk;
    ModemPSK psk;
    ModemQAM qam;
    ModemSQAM sqam;
    std::vector<std::pair<Modem*, std::string>> modems = {
        {&ask, "16"}, {&apsk, "32"}, {&dpsk, "8"},
        {&psk, "64"}, {&qam, "16"}, {&sqam, "128"}
    };
    ModemIQData input = makeDigitalSignal(64);
    AudioThreadInput output;

    for (auto& entry : modems) {
        entry.first->writeSetting("cons", entry.second);
        ModemKit *kit = entry.first->buildKit(19'200, 48'000);
        entry.first->demodulate(kit, &input, &output);
        entry.first->disposeKit(kit);
    }
}

CUBIC_TEST(fsk_modem_validates_rates_settings_and_streaming_chunks) {
    ModemFSK modem;
    CUBIC_REQUIRE(modem.getName() == "FSK");
    CUBIC_REQUIRE(modem.getSettings().size() == 3);
    CUBIC_REQUIRE(modem.checkSampleRate(9'600, 48'000) == 19'200);
    modem.writeSetting("bps", "2");
    modem.writeSetting("sps", "4800");
    modem.writeSetting("bw", "0.35");
    CUBIC_REQUIRE(modem.shouldRebuildKit());
    CUBIC_REQUIRE(modem.readSetting("bps") == "2");
    CUBIC_REQUIRE(modem.readSetting("sps") == "4800");
    modem.writeSetting("bps", "4");
    CUBIC_REQUIRE(modem.checkSampleRate(19'200, 48'000) == 76'800);

    ModemKit *kit = modem.buildKit(76'800, 48'000);
    ModemIQData input = makeDigitalSignal(32, 76'800);
    AudioThreadInput output;
    modem.demodulate(kit, &input, &output);
    modem.disposeKit(kit);
}

CUBIC_TEST(gmsk_modem_validates_settings_and_streaming_chunks) {
    ModemGMSK modem;
    CUBIC_REQUIRE(modem.getName() == "GMSK");
    CUBIC_REQUIRE(modem.getSettings().size() == 3);
    CUBIC_REQUIRE(modem.checkSampleRate(100, 48'000) == MIN_BANDWIDTH);
    modem.writeSetting("fdelay", "4");
    modem.writeSetting("sps", "8");
    modem.writeSetting("ebf", "0.25");
    CUBIC_REQUIRE(modem.shouldRebuildKit());
    CUBIC_REQUIRE(modem.readSetting("fdelay") == "4");
    CUBIC_REQUIRE(modem.readSetting("sps") == "8");

    ModemKit *kit = modem.buildKit(19'200, 48'000);
    ModemIQData input = makeDigitalSignal(64);
    AudioThreadInput output;
    modem.demodulate(kit, &input, &output);
    modem.disposeKit(kit);
}

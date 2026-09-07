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
template <typename T> void exerciseAllConstellations(const std::string& name) {
    std::unique_ptr<Modem> modem(static_cast<Modem*>(T::factory()));
    CUBIC_REQUIRE(modem->getName() == name);
    CUBIC_REQUIRE(modem->getDefaultSampleRate() > 0);
    CUBIC_REQUIRE(modem->checkSampleRate(1, 48000) == MIN_BANDWIDTH);
    auto settings = modem->readSettings();
    modem->writeSettings(settings);
    modem->writeSetting("unknown", "ignored");
    CUBIC_REQUIRE(modem->readSetting("unknown").empty());
    auto options = modem->getSettings().front().options;
    ModemIQData input;
    input.sampleRate = 19200;
    input.data.resize(256);
    for (auto& sample : input.data) { sample.real = 0.5f; sample.imag = 0.5f; }
    ModemKit* kit = modem->buildKit(19200, 48000);
    for (const auto& option : options) {
        modem->writeSetting("cons", option);
        CUBIC_REQUIRE(modem->readSetting("cons") == option);
        input.discontinuity = true;
        modem->demodulate(kit, &input, nullptr);
        auto* digitalKit = static_cast<ModemKitDigital*>(kit);
        CUBIC_REQUIRE(digitalKit->symbolTracker != nullptr);
        input.discontinuity = false;
        modem->demodulate(kit, &input, nullptr);
    }
    input.data.clear();
    modem->demodulate(kit, &input, nullptr);
    modem->disposeKit(kit);
}
}

CUBIC_TEST(digital_modems_support_every_advertised_constellation_and_tracker_rebuild) {
    exerciseAllConstellations<ModemASK>("ASK");
    exerciseAllConstellations<ModemAPSK>("APSK");
    exerciseAllConstellations<ModemDPSK>("DPSK");
    exerciseAllConstellations<ModemPSK>("PSK");
    exerciseAllConstellations<ModemQAM>("QAM");
    exerciseAllConstellations<ModemSQAM>("SQAM");
}

CUBIC_TEST(digital_tracking_settings_clamp_and_fixed_factories_reset) {
    for (auto factory : {ModemBPSK::factory, ModemQPSK::factory, ModemOOK::factory, ModemST::factory}) {
        std::unique_ptr<ModemDigital> modem(static_cast<ModemDigital*>(factory()));
        modem->writeSetting("sps", "1");
        CUBIC_REQUIRE(modem->readSetting("sps") == "2");
        modem->writeSetting("beta", "0");
        CUBIC_REQUIRE_NEAR(std::stof(modem->readSetting("beta")), 0.05, 1e-6);
        modem->writeSetting("beta", "2");
        CUBIC_REQUIRE(std::stof(modem->readSetting("beta")) == 1.0f);
        auto* kit = modem->buildKit(19200, 48000);
        ModemIQData input;
        input.discontinuity = true;
        input.data.resize(64);
        for (auto& sample : input.data) { sample.real = 1; sample.imag = 0; }
        modem->demodulate(kit, &input, nullptr);
        input.data.clear();
        modem->demodulate(kit, &input, nullptr);
        modem->disposeKit(kit);
    }
}

CUBIC_TEST(fsk_and_gmsk_output_complete_symbols_across_packets_and_reset) {
    struct Output : ModemDigitalOutput {
        std::string text;
        void write(std::string value) override { text += value; }
        void write(char value) override { text += value; }
        void Show() override {}
        void Hide() override {}
        void Close() override {}
    } output;
    for (auto factory : {ModemFSK::factory, ModemGMSK::factory}) {
        std::unique_ptr<ModemDigital> modem(static_cast<ModemDigital*>(factory()));
        modem->setOutput(&output);
        modem->writeSetting("unknown", "1");
        CUBIC_REQUIRE(modem->readSetting("unknown").empty());
        CUBIC_REQUIRE(!modem->readSettings().empty());
        auto rate = modem->checkSampleRate(modem->getDefaultSampleRate(), 48000);
        auto* kit = modem->buildKit(rate, 48000);
        ModemIQData input;
        input.sampleRate = rate;
        input.data.resize(1);
        input.data[0].real = 1;
        input.data[0].imag = 0;
        output.text.clear();
        modem->demodulate(kit, &input, nullptr);
        CUBIC_REQUIRE(output.text.empty());
        input.discontinuity = true;
        modem->demodulate(kit, &input, nullptr);
        CUBIC_REQUIRE(output.text.empty());
        input.discontinuity = false;
        input.data.resize(128, input.data.front());
        modem->demodulate(kit, &input, nullptr);
        CUBIC_REQUIRE(!output.text.empty());
        CUBIC_REQUIRE(output.text.find_first_not_of("0123456789") == std::string::npos);
        modem->setOutput(nullptr);
        modem->demodulate(kit, &input, nullptr);
        modem->disposeKit(kit);
    }
}

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

// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "ModemIQ.h"

#include <memory>

CUBIC_TEST(modem_iq_interleaves_quadrature_and_in_phase_audio) {
    ModemIQ modem;
    ModemIQData input;
    input.data.resize(2);
    input.data[0].real = 1.0f;
    input.data[0].imag = 2.0f;
    input.data[1].real = -3.0f;
    input.data[1].imag = -4.0f;
    AudioThreadInput output;

    modem.demodulate(nullptr, &input, &output);

    CUBIC_REQUIRE(output.channels == 2);
    CUBIC_REQUIRE(output.data.size() == 4);
    CUBIC_REQUIRE(output.data[0] == 2.0f);
    CUBIC_REQUIRE(output.data[1] == 1.0f);
    CUBIC_REQUIRE(output.data[2] == -4.0f);
    CUBIC_REQUIRE(output.data[3] == -3.0f);
}

CUBIC_TEST(modem_iq_builds_and_disposes_its_rate_kit) {
    ModemIQ modem;
    ModemKit *kit = modem.buildKit(192'000, 48'000);

    CUBIC_REQUIRE(kit != nullptr);
    CUBIC_REQUIRE(kit->sampleRate == 192'000);
    CUBIC_REQUIRE(kit->audioSampleRate == 48'000);
    CUBIC_REQUIRE(modem.checkSampleRate(192'000, 48'000) == 48'000);
    CUBIC_REQUIRE(modem.getDefaultSampleRate() == 48'000);

    modem.disposeKit(kit);
}

CUBIC_TEST(modem_registry_creates_registered_modems) {
    Modem::addModemFactory(&ModemIQ::factory, "I/Q test", 48'000);

    std::unique_ptr<Modem> modem(Modem::makeModem("I/Q test"));
    CUBIC_REQUIRE(modem != nullptr);
    CUBIC_REQUIRE(modem->getName() == "I/Q");
    CUBIC_REQUIRE(Modem::getModemDefaultSampleRate("I/Q test") == 48'000);
    CUBIC_REQUIRE(Modem::makeModem("missing modem") == nullptr);
}

CUBIC_TEST(modem_rebuild_state_is_initialized_and_explicit) {
    ModemIQ modem;

    CUBIC_REQUIRE(!modem.shouldRebuildKit());
    modem.rebuildKit();
    CUBIC_REQUIRE(modem.shouldRebuildKit());
    modem.clearRebuildKit();
    CUBIC_REQUIRE(!modem.shouldRebuildKit());
}

CUBIC_TEST(modem_range_reports_its_bounds) {
    ModemRange range(-12.5, 42.0);

    CUBIC_REQUIRE(range.minimum() == -12.5);
    CUBIC_REQUIRE(range.maximum() == 42.0);
}

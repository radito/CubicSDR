// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "AudioThread.h"
#include "DemodDefs.h"
#include "SDRDeviceInfo.h"
#include <SoapySDR/Registry.hpp>
#include <algorithm>

namespace {
class TestRadio : public SoapySDR::Device {
public:
    std::vector<double> rates = {96000, 24000, 48000};
    bool correction = true;
    std::vector<double> listSampleRates(int, size_t) const override { return rates; }
    std::vector<std::string> listFrequencies(int, size_t) const override {
        return correction ? std::vector<std::string>{"RF", "CORR"} : std::vector<std::string>{"RF"};
    }
    std::vector<std::string> listAntennas(int, size_t) const override { return {"A", "B"}; }
    std::string getAntenna(int, size_t) const override { return "B"; }
    std::vector<std::string> listGains(int, size_t) const override { return {"RF"}; }
    SoapySDR::Range getGainRange(int, size_t, const std::string&) const override { return {0, 40, 2}; }
    double getGain(int, size_t, const std::string&) const override { return 12; }
};
SoapySDR::KwargsList findTestRadio(const SoapySDR::Kwargs& args) { return {args}; }
SoapySDR::Device* makeTestRadio(const SoapySDR::Kwargs&) { return new TestRadio; }
}

CUBIC_TEST(sdr_device_capabilities_use_a_fake_driver_without_radio_hardware) {
    SoapySDR::Registry registration("cubicsdr_unit_test", findTestRadio, makeTestRadio, SOAPY_SDR_ABI_VERSION);
    SDRDeviceInfo info;
    info.setDeviceArgs({{"driver", "cubicsdr_unit_test"}});
    auto* radio = static_cast<TestRadio*>(info.getSoapyDevice());
    CUBIC_REQUIRE(radio != nullptr);
    CUBIC_REQUIRE(info.getSoapyDevice() == radio);
    CUBIC_REQUIRE(info.hasCORR(SOAPY_SDR_RX, 0));
    radio->correction = false;
    CUBIC_REQUIRE(!info.hasCORR(SOAPY_SDR_RX, 0));
    CUBIC_REQUIRE(info.getSampleRates(SOAPY_SDR_RX, 0) == (std::vector<long>{24000, 48000, 96000}));
    CUBIC_REQUIRE(info.getSampleRateNear(SOAPY_SDR_RX, 0, 47000) == 48000);
    CUBIC_REQUIRE(info.getSampleRateNear(SOAPY_SDR_RX, 0, 1) == 24000);
    CUBIC_REQUIRE(info.getSampleRateNear(SOAPY_SDR_RX, 0, 200000) == 96000);
    CUBIC_REQUIRE(info.getAntennaNames(SOAPY_SDR_RX, 0) == (std::vector<std::string>{"A", "B"}));
    CUBIC_REQUIRE(info.getAntennaName(SOAPY_SDR_RX, 0) == "B");
    auto gains = info.getGains(SOAPY_SDR_RX, 0);
    CUBIC_REQUIRE(gains.size() == 1);
    CUBIC_REQUIRE(gains.at("RF").minimum() == 0);
    CUBIC_REQUIRE(gains.at("RF").maximum() == 40);
    CUBIC_REQUIRE(gains.at("RF").step() == 2);
    CUBIC_REQUIRE(info.getCurrentGain(SOAPY_SDR_RX, 0, "RF") == 12);
    CUBIC_REQUIRE(info.getCurrentGain(SOAPY_SDR_RX, 0, "missing") == 0);
    radio->rates.clear();
    for (int i = 1000; i >= 1; --i) radio->rates.push_back(i * 1000.0);
    auto rates = info.getSampleRates(SOAPY_SDR_RX, 0);
    CUBIC_REQUIRE(std::is_sorted(rates.begin(), rates.end()));
    CUBIC_REQUIRE(rates.size() < radio->rates.size());
    CUBIC_REQUIRE(rates.front() == 1000 && rates.back() == 1000000);
    auto* replacement = SoapySDR::Device::make(SoapySDR::Kwargs{{"driver", "cubicsdr_unit_test"}, {"serial", "second"}});
    info.setSoapyDevice(replacement);
    CUBIC_REQUIRE(info.getSoapyDevice() == replacement);
}

CUBIC_TEST(sdr_device_info_round_trips_identity_state_and_arguments) {
    SDRDeviceInfo device;
    CUBIC_REQUIRE(!device.isAvailable());
    CUBIC_REQUIRE(!device.isActive());
    CUBIC_REQUIRE(!device.isRemote());
    CUBIC_REQUIRE(!device.isManual());
    CUBIC_REQUIRE(!device.hasTimestamps());

    device.setIndex(3);
    device.setName("Airspy HF+");
    device.setSerial("ABC123");
    device.setTuner("HF");
    device.setManufacturer("Airspy");
    device.setProduct("Discovery");
    device.setDriver("airspyhf");
    device.setHardware("airspyhf");
    device.setAvailable(true);
    device.setActive(true);
    device.setRemote(true);
    device.setManual(true);
    device.setTimestamps(true);
    device.setManualParams("serial=ABC123");
    device.setDeviceArgs({{"driver", "airspyhf"}, {"serial", "ABC123"}});
    device.setStreamArgs({{"buffers", "8"}});

    CUBIC_REQUIRE(device.getIndex() == 3);
    CUBIC_REQUIRE(device.getDeviceId() == "Airspy HF+");
    CUBIC_REQUIRE(device.getSerial() == "ABC123");
    CUBIC_REQUIRE(device.getTuner() == "HF");
    CUBIC_REQUIRE(device.getManufacturer() == "Airspy");
    CUBIC_REQUIRE(device.getProduct() == "Discovery");
    CUBIC_REQUIRE(device.getDriver() == "airspyhf");
    CUBIC_REQUIRE(device.getHardware() == "airspyhf");
    CUBIC_REQUIRE(device.isAvailable());
    CUBIC_REQUIRE(device.isActive());
    CUBIC_REQUIRE(device.isRemote());
    CUBIC_REQUIRE(device.isManual());
    CUBIC_REQUIRE(device.hasTimestamps());
    CUBIC_REQUIRE(device.getManualParams() == "serial=ABC123");
    CUBIC_REQUIRE(device.getDeviceArgs().at("driver") == "airspyhf");
    CUBIC_REQUIRE(device.getStreamArgs().at("buffers") == "8");
}

CUBIC_TEST(audio_thread_input_copy_preserves_playback_metadata_and_samples) {
    AudioThreadInput source;
    source.frequency = 101700000;
    source.inputRate = 192000;
    source.sampleRate = 48000;
    source.channels = 2;
    source.peak = 0.75f;
    source.type = 4;
    source.is_squelch_active = true;
    source.is_denoised = true;
    source.discontinuity = true;
    source.data = {0.1f, -0.2f, 0.3f, -0.4f};

    AudioThreadInput copy(&source);
    CUBIC_REQUIRE(copy.frequency == source.frequency);
    CUBIC_REQUIRE(copy.inputRate == source.inputRate);
    CUBIC_REQUIRE(copy.sampleRate == source.sampleRate);
    CUBIC_REQUIRE(copy.channels == source.channels);
    CUBIC_REQUIRE(copy.peak == source.peak);
    CUBIC_REQUIRE(copy.type == source.type);
    CUBIC_REQUIRE(copy.is_squelch_active);
    CUBIC_REQUIRE(copy.is_denoised);
    CUBIC_REQUIRE(copy.discontinuity);
    CUBIC_REQUIRE(copy.data == source.data);

    source.data[0] = 1.0f;
    CUBIC_REQUIRE(copy.data[0] == 0.1f);
}

CUBIC_TEST(demodulator_iq_assignment_deep_copies_samples_and_metadata) {
    DemodulatorThreadIQData source;
    source.frequency = 144390000;
    source.sampleRate = 24000;
    source.discontinuity = false;
    source.hasTimestamp = true;
    source.sequence = 42;
    source.timeNs = 123456789;
    source.data.resize(2);
    source.data[0].real = 1.0f;
    source.data[1].imag = -1.0f;

    DemodulatorThreadIQData copy;
    copy = source;
    CUBIC_REQUIRE(copy.frequency == source.frequency);
    CUBIC_REQUIRE(copy.sampleRate == source.sampleRate);
    CUBIC_REQUIRE(!copy.discontinuity);
    CUBIC_REQUIRE(copy.hasTimestamp);
    CUBIC_REQUIRE(copy.sequence == 42);
    CUBIC_REQUIRE(copy.timeNs == 123456789);
    CUBIC_REQUIRE(copy.data.size() == 2);
    CUBIC_REQUIRE(copy.data[0].real == 1.0f);
    CUBIC_REQUIRE(copy.data[1].imag == -1.0f);

    source.data[0].real = 5.0f;
    CUBIC_REQUIRE(copy.data[0].real == 1.0f);
}

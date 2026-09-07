// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "ModemDigital.h"

#include <algorithm>

ModemDigitalOutput::ModemDigitalOutput() = default;

ModemDigital::ModemDigital() : Modem(), currentDemodLock(false) {
#if ENABLE_DIGITAL_LAB
    digitalOut = nullptr;
#endif
}

ModemDigitalOutput::~ModemDigitalOutput() = default;

std::string ModemDigital::getType() {
    return "digital";
}

int ModemDigital::checkSampleRate(long long sampleRate, int /* audioSampleRate */) {
    if (sampleRate < MIN_BANDWIDTH) {
        return MIN_BANDWIDTH;
    }
    return (int)sampleRate;
}

ModemArgInfoList ModemDigital::getSettings() {
    ModemArgInfo spsArg;
    spsArg.key = "sps";
    spsArg.name = "Samples / symbol";
    spsArg.value = std::to_string(samplesPerSymbol);
    spsArg.description = "Input samples per digital symbol";
    spsArg.type = ModemArgInfo::Type::INT;
    spsArg.units = "samples/symbol";
    spsArg.range = ModemRange(2, 64);

    ModemArgInfo betaArg;
    betaArg.key = "beta";
    betaArg.name = "Excess bandwidth";
    betaArg.value = std::to_string(excessBandwidth);
    betaArg.description = "Root-raised-cosine excess bandwidth";
    betaArg.type = ModemArgInfo::Type::FLOAT;
    betaArg.range = ModemRange(0.05, 1.0);
    return {spsArg, betaArg};
}

void ModemDigital::writeSetting(std::string setting, std::string value) {
    if (setting == "sps") {
        samplesPerSymbol = std::max(2, std::stoi(value));
        rebuildKit();
    } else if (setting == "beta") {
        excessBandwidth = std::clamp(std::stof(value), 0.05f, 1.0f);
        rebuildKit();
    }
}

std::string ModemDigital::readSetting(std::string setting) {
    if (setting == "sps") return std::to_string(samplesPerSymbol);
    if (setting == "beta") return std::to_string(excessBandwidth);
    return "";
}

ModemKit *ModemDigital::buildKit(long long sampleRate, int audioSampleRate) {
    auto *dkit = new ModemKitDigital;
    
    dkit->sampleRate = sampleRate;
    dkit->audioSampleRate = audioSampleRate;
    dkit->samplesPerSymbol = static_cast<unsigned int>(samplesPerSymbol);
    dkit->excessBandwidth = excessBandwidth;
    
    return dkit;
}

void ModemDigital::disposeKit(ModemKit *kit) {
    auto *dkit = (ModemKitDigital *)kit;
    if (dkit->symbolTracker) symtrack_cccf_destroy(dkit->symbolTracker);
    delete dkit;
}

void ModemDigital::setDemodulatorLock(bool demod_lock_in) {
    currentDemodLock.store(demod_lock_in);
}

int ModemDigital::getDemodulatorLock() {
    return currentDemodLock.load();
}

void ModemDigital::updateDemodulatorLock(modemcf mod, float sensitivity) {
    setDemodulatorLock(modemcf_get_demodulator_evm(mod) <= sensitivity);
}

void ModemDigital::digitalStart(ModemKitDigital *kit, modemcf mod, ModemIQData *input) {
    synchronizedInput.clear();
    if (mod != nullptr) {
        const modulation_scheme scheme = modemcf_get_scheme(mod);
        // Liquid represents constellations such as V.29 internally as
        // LIQUID_MODEM_ARB.  That value cannot be passed back to
        // symtrack_cccf_create() without a custom constellation table.
        const bool canTrack = scheme != LIQUID_MODEM_UNKNOWN &&
                              scheme != LIQUID_MODEM_ARB;
        if (!canTrack) {
            if (kit->symbolTracker) {
                symtrack_cccf_destroy(kit->symbolTracker);
                kit->symbolTracker = nullptr;
            }
            kit->trackerScheme = LIQUID_MODEM_UNKNOWN;
            synchronizedInput.assign(input->data.begin(), input->data.end());
        } else if (!kit->symbolTracker || kit->trackerScheme != scheme) {
            if (kit->symbolTracker) symtrack_cccf_destroy(kit->symbolTracker);
            kit->symbolTracker = symtrack_cccf_create(
                LIQUID_FIRFILT_RRC, kit->samplesPerSymbol, 7,
                kit->excessBandwidth, scheme);
            if (kit->symbolTracker) {
                symtrack_cccf_set_bandwidth(kit->symbolTracker, 0.01f);
                kit->trackerScheme = scheme;
            } else {
                kit->trackerScheme = LIQUID_MODEM_UNKNOWN;
            }
        }
        if (input->discontinuity) {
            modemcf_reset(mod);
            if (kit->symbolTracker) symtrack_cccf_reset(kit->symbolTracker);
            setDemodulatorLock(false);
        }

        if (canTrack && kit->symbolTracker) {
            synchronizedInput.resize(input->data.size() * 2 + 2);
            unsigned int written = 0;
            if (!input->data.empty()) {
                symtrack_cccf_execute_block(kit->symbolTracker, input->data.data(),
                    static_cast<unsigned int>(input->data.size()),
                    synchronizedInput.data(), &written);
            }
            synchronizedInput.resize(written);
        } else if (canTrack) {
            synchronizedInput.assign(input->data.begin(), input->data.end());
        }
    }
    const size_t bufSize = synchronizedInput.size();
    
    if (demodOutputDataDigital.size() != bufSize) {
        if (demodOutputDataDigital.capacity() < bufSize) {
            demodOutputDataDigital.reserve(bufSize);
        }
        demodOutputDataDigital.resize(bufSize);
    }
}

void ModemDigital::digitalFinish(ModemKitDigital * /* kit */, modemcf /* mod */) {
#if ENABLE_DIGITAL_LAB
    if (digitalOut && outStream.str().length()) {
        digitalOut->write(outStream.str());
        outStream.str("");
    } else {
        outStream.str("");
    }
#endif
}

#if ENABLE_DIGITAL_LAB
void ModemDigital::setOutput(ModemDigitalOutput *modemDigitalOutput) {
    digitalOut = modemDigitalOutput;
}
#endif

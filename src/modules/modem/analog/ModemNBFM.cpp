// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "ModemNBFM.h"

namespace {
iirfilt_rrrf createNBFMDeemphasisFilter(int microseconds, int sampleRate) {
    if (microseconds <= 0 || sampleRate <= 0) return nullptr;
    const double tau = static_cast<double>(microseconds) * 1.0e-6;
    const double alpha = std::exp(-1.0 / (tau * static_cast<double>(sampleRate)));
    float b[2] = {static_cast<float>(1.0 - alpha), 0.0f};
    float a[2] = {1.0f, static_cast<float>(-alpha)};
    return iirfilt_rrrf_create(b, 2, a, 2);
}
}

ModemNBFM::ModemNBFM() : ModemAnalog(), demph(0) {
    demodFM = freqdem_create(0.5);
}

ModemNBFM::~ModemNBFM() {
    freqdem_destroy(demodFM);
}

ModemBase *ModemNBFM::factory() {
    return new ModemNBFM;
}

std::string ModemNBFM::getName() {
    return "NBFM";
}

int ModemNBFM::getDefaultSampleRate() {
    return 12500;
}

ModemArgInfoList ModemNBFM::getSettings() {
    ModemArgInfo arg;
    arg.key = "demph";
    arg.name = "De-emphasis";
    arg.value = std::to_string(demph);
    arg.description = "Narrow-band FM de-emphasis time constant";
    arg.type = ModemArgInfo::Type::STRING;
    arg.options = {"0", "300", "750"};
    arg.optionNames = {"None", "300us", "750us"};
    return {arg};
}

void ModemNBFM::writeSetting(std::string setting, std::string value) {
    if (setting == "demph") {
        demph = std::stoi(value);
        rebuildKit();
    }
}

std::string ModemNBFM::readSetting(std::string setting) {
    return setting == "demph" ? std::to_string(demph) : "";
}

ModemKit *ModemNBFM::buildKit(long long sampleRate, int audioSampleRate) {
    auto *kit = new ModemKitNBFM;
    kit->sampleRate = sampleRate;
    kit->audioSampleRate = audioSampleRate;
    kit->audioResampleRatio = static_cast<double>(audioSampleRate) / static_cast<double>(sampleRate);
    kit->audioResampler = msresamp_rrrf_create(static_cast<float>(kit->audioResampleRatio), 60.0f);
    kit->deemphasis = createNBFMDeemphasisFilter(demph, static_cast<int>(sampleRate));
    return kit;
}

void ModemNBFM::disposeKit(ModemKit *kitIn) {
    auto *kit = static_cast<ModemKitNBFM *>(kitIn);
    if (kit->deemphasis) iirfilt_rrrf_destroy(kit->deemphasis);
    msresamp_rrrf_destroy(kit->audioResampler);
    delete kit;
}

void ModemNBFM::demodulate(ModemKit *kit, ModemIQData *input, AudioThreadInput *audioOut) {
    auto *fmkit = static_cast<ModemKitNBFM *>(kit);
    
    initOutputBuffers(fmkit, input);
    
    if (!bufSize) {
       
        return;
    }


    if (input->discontinuity) {
        freqdem_reset(demodFM);
        if (fmkit->deemphasis) iirfilt_rrrf_reset(fmkit->deemphasis);
    }
    
    freqdem_demodulate_block(demodFM, &input->data[0], (unsigned int)bufSize, &demodOutputData[0]);

    if (fmkit->deemphasis) {
        for (size_t i = 0; i < bufSize; ++i) {
            iirfilt_rrrf_execute(fmkit->deemphasis, demodOutputData[i], &demodOutputData[i]);
        }
    }

    buildAudioOutput(fmkit, audioOut, false);
}

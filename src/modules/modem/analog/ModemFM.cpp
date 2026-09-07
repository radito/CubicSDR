// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "ModemFM.h"

namespace {
iirfilt_rrrf createDeemphasisFilter(int microseconds, int sampleRate) {
    if (microseconds <= 0 || sampleRate <= 0) return nullptr;
    const double tau = static_cast<double>(microseconds) * 1.0e-6;
    const double alpha = std::exp(-1.0 / (tau * static_cast<double>(sampleRate)));
    float b[2] = {static_cast<float>(1.0 - alpha), 0.0f};
    float a[2] = {1.0f, static_cast<float>(-alpha)};
    return iirfilt_rrrf_create(b, 2, a, 2);
}
}

ModemFM::ModemFM() : ModemAnalog(), demph(75) {
    demodFM = freqdem_create(0.5);
}

ModemFM::~ModemFM() {
    freqdem_destroy(demodFM);
}

ModemBase *ModemFM::factory() {
    return new ModemFM;
}

std::string ModemFM::getName() {
    return "FM";
}

int ModemFM::getDefaultSampleRate() {
    return 200000;
}

ModemArgInfoList ModemFM::getSettings() {
    ModemArgInfo arg;
    arg.key = "demph";
    arg.name = "De-emphasis";
    arg.value = std::to_string(demph);
    arg.description = "FM de-emphasis time constant";
    arg.type = ModemArgInfo::Type::STRING;
    arg.options = {"0", "50", "75"};
    arg.optionNames = {"None", "50us", "75us"};
    return {arg};
}

void ModemFM::writeSetting(std::string setting, std::string value) {
    if (setting == "demph") {
        demph = std::stoi(value);
        rebuildKit();
    }
}

std::string ModemFM::readSetting(std::string setting) {
    return setting == "demph" ? std::to_string(demph) : "";
}

ModemKit *ModemFM::buildKit(long long sampleRate, int audioSampleRate) {
    auto *kit = new ModemKitFM;
    kit->sampleRate = sampleRate;
    kit->audioSampleRate = audioSampleRate;
    kit->audioResampleRatio = static_cast<double>(audioSampleRate) / static_cast<double>(sampleRate);
    kit->audioResampler = msresamp_rrrf_create(static_cast<float>(kit->audioResampleRatio), 60.0f);
    kit->deemphasis = createDeemphasisFilter(demph, static_cast<int>(sampleRate));
    return kit;
}

void ModemFM::disposeKit(ModemKit *kitIn) {
    auto *kit = static_cast<ModemKitFM *>(kitIn);
    if (kit->deemphasis) iirfilt_rrrf_destroy(kit->deemphasis);
    msresamp_rrrf_destroy(kit->audioResampler);
    delete kit;
}

void ModemFM::demodulate(ModemKit *kit, ModemIQData *input, AudioThreadInput *audioOut) {
    auto *fmkit = static_cast<ModemKitFM *>(kit);
    
    initOutputBuffers(fmkit, input);
    
    if (!bufSize) {
      
        return;
    }


    if (input->discontinuity) {
        freqdem_reset(demodFM);
        if (fmkit->deemphasis) iirfilt_rrrf_reset(fmkit->deemphasis);
    }
    
    freqdem_demodulate_block(demodFM, &input->data[0], (int)bufSize, &demodOutputData[0]);

    if (fmkit->deemphasis) {
        for (size_t i = 0; i < bufSize; ++i) {
            iirfilt_rrrf_execute(fmkit->deemphasis, demodOutputData[i], &demodOutputData[i]);
        }
    }

    buildAudioOutput(fmkit, audioOut, false);
}

// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#pragma once
#include "Modem.h"
#include "ModemAnalog.h"

class ModemKitFM : public ModemKitAnalog {
public:
    iirfilt_rrrf deemphasis = nullptr;
};

class ModemFM : public ModemAnalog {
public:
    ModemFM();
    ~ModemFM() override;
    
    std::string getName() override;
    
    static ModemBase *factory();

    int getDefaultSampleRate() override;
    ModemArgInfoList getSettings() override;
    void writeSetting(std::string setting, std::string value) override;
    std::string readSetting(std::string setting) override;
    ModemKit *buildKit(long long sampleRate, int audioSampleRate) override;
    void disposeKit(ModemKit *kit) override;

    void demodulate(ModemKit *kit, ModemIQData *input, AudioThreadInput *audioOut) override;

private:
    freqdem demodFM;
    int demph;
};

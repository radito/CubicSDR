// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "RDSDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr uint64_t RDS_SEARCH_TIMEOUT_SAMPLES = 6 * 19000;
constexpr const char *PTY_NAMES[32] = {
    "None", "News", "Current Affairs", "Information", "Sport", "Education",
    "Drama", "Culture", "Science", "Varied", "Pop Music", "Rock Music",
    "Easy Listening", "Light Classics", "Serious Classics", "Other Music",
    "Weather", "Finance", "Children", "Social Affairs", "Religion", "Phone In",
    "Travel", "Leisure", "Jazz", "Country", "National Music", "Oldies",
    "Folk", "Documentary", "Alarm Test", "Alarm"
};
}

RDSDecoder::RDSDecoder() {
    reset();
}

void RDSDecoder::reset() {
    resetSignal();
    programId = 0;
    programService.fill(' ');
    serviceSegments = 0;
    programType = 0;
    trafficProgram = false;
    trafficAnnouncement = false;
    synchronized = false;
    statusText = "RDS -";
    updatePending = true;
}

void RDSDecoder::resetSignal() {
    phases = {};
    directBlocks = {};
    sampleIndex = 0;
    lastValidGroupSample = 0;
}

void RDSDecoder::process(const liquid_float_complex *samples, size_t count) {
    if (!samples) return;
    for (size_t i = 0; i < count; ++i, ++sampleIndex) {
        for (size_t phaseIndex = 0; phaseIndex < phases.size(); ++phaseIndex) {
            auto& phase = phases[phaseIndex];
            const size_t position = (sampleIndex + SAMPLES_PER_BIT - phaseIndex) %
                                    SAMPLES_PER_BIT;
            if (position < SAMPLES_PER_BIT / 2) {
                phase.first.real += samples[i].real;
                phase.first.imag += samples[i].imag;
            } else {
                phase.second.real += samples[i].real;
                phase.second.imag += samples[i].imag;
            }
            if (position == SAMPLES_PER_BIT - 1) {
                liquid_float_complex symbol;
                symbol.real = phase.first.real - phase.second.real;
                symbol.imag = phase.first.imag - phase.second.imag;
                phase.first = {};
                phase.second = {};
                if (phase.primed) processSymbol(phase, symbol);
                phase.primed = true;
            }
        }
    }
    if (synchronized && sampleIndex - lastValidGroupSample > RDS_SEARCH_TIMEOUT_SAMPLES) {
        synchronized = false;
        for (auto& phase : phases) phase.blocks.validGroups = 0;
        publishSearching();
    }
}

void RDSDecoder::processSymbol(PhaseState& phase, liquid_float_complex symbol) {
    if (symbol.real * symbol.real + symbol.imag * symbol.imag < 1.0e-12f) {
        phase.havePrevious = false;
        return;
    }
    if (phase.havePrevious) {
        const float differential = symbol.real * phase.previous.real +
                                   symbol.imag * phase.previous.imag;
        processRecoveredBit(phase.blocks, differential < 0.0f);
    }
    phase.previous = symbol;
    phase.havePrevious = true;
}

void RDSDecoder::processBit(bool bit) {
    processRecoveredBit(directBlocks, bit);
}

bool RDSDecoder::takeUpdate(std::string& status) {
    if (!updatePending) return false;
    status = statusText;
    updatePending = false;
    return true;
}

uint16_t RDSDecoder::syndrome(uint32_t block) {
    constexpr uint32_t polynomial = 0x5B9;
    for (int bit = 25; bit >= 10; --bit) {
        if (block & (1u << bit)) block ^= polynomial << (bit - 10);
    }
    return static_cast<uint16_t>(block & 0x3FFu);
}

void RDSDecoder::processRecoveredBit(BlockState& state, bool bit) {
    constexpr uint16_t expectedSyndromes[4] = {0x0FC, 0x198, 0x168, 0x1B4};
    state.shift = ((state.shift << 1) | static_cast<uint32_t>(bit)) & 0x03FFFFFFu;
    const uint16_t currentSyndrome = syndrome(state.shift);

    if (state.expectedBlock < 0) {
        if (currentSyndrome == expectedSyndromes[0]) {
            state.words[0] = static_cast<uint16_t>(state.shift >> 10);
            state.expectedBlock = 1;
            state.countdown = 26;
        }
        return;
    }

    if (--state.countdown > 0) return;
    const int blockIndex = state.expectedBlock;
    const auto matchesExpected = [&](uint16_t value) {
        return blockIndex == 2
            ? (value == expectedSyndromes[2] || value == 0x350)
            : value == expectedSyndromes[blockIndex];
    };
    uint32_t decodedBlock = state.shift;
    bool valid = matchesExpected(currentSyndrome);
    if (!valid) {
        // RDS checkwords can safely recover a single damaged bit. This avoids
        // dropping an entire group for a short noise impulse.
        for (int bitIndex = 0; bitIndex < 26; ++bitIndex) {
            const uint32_t candidate = state.shift ^ (1u << bitIndex);
            if (matchesExpected(syndrome(candidate))) {
                decodedBlock = candidate;
                valid = true;
                break;
            }
        }
    }

    if (!valid) {
        state.expectedBlock = -1;
        state.validGroups = 0;
        if (currentSyndrome == expectedSyndromes[0]) {
            state.words[0] = static_cast<uint16_t>(state.shift >> 10);
            state.expectedBlock = 1;
            state.countdown = 26;
        }
        return;
    }

    state.words[blockIndex] = static_cast<uint16_t>(decodedBlock >> 10);
    if (blockIndex == 3) {
        state.validGroups = std::min(state.validGroups + 1, 1000);
        acceptGroup(state);
        state.expectedBlock = 0;
    } else if (blockIndex == 0) {
        state.expectedBlock = 1;
    } else {
        state.expectedBlock++;
    }
    state.countdown = 26;
}

void RDSDecoder::acceptGroup(BlockState& state) {
    lastValidGroupSample = sampleIndex;
    const uint16_t newProgramId = state.words[0];
    if (programId != newProgramId) {
        programId = newProgramId;
        programService.fill(' ');
        serviceSegments = 0;
    }

    const uint16_t blockB = state.words[1];
    const unsigned int groupType = (blockB >> 12) & 0x0F;
    trafficProgram = (blockB & 0x0400) != 0;
    programType = static_cast<uint8_t>((blockB >> 5) & 0x1F);
    if (groupType == 0) {
        trafficAnnouncement = (blockB & 0x0010) != 0;
        const unsigned int segment = blockB & 0x03;
        const uint16_t blockD = state.words[3];
        const char first = static_cast<char>(blockD >> 8);
        const char second = static_cast<char>(blockD & 0xFF);
        programService[segment * 2] = first >= 32 && first <= 126 ? first : ' ';
        programService[segment * 2 + 1] = second >= 32 && second <= 126 ? second : ' ';
        serviceSegments |= static_cast<uint8_t>(1u << segment);
    }
    publishStatus(state.validGroups);
}

void RDSDecoder::publishStatus(int validGroups) {
    if (validGroups < 2 || !programId) return;
    synchronized = true;

    char id[5];
    std::snprintf(id, sizeof(id), "%04X", programId);
    std::string next = "RDS SYNC ";
    next += id;
    if (serviceSegments == 0x0F) {
        std::string service(programService.begin(), programService.end());
        while (!service.empty() && service.back() == ' ') service.pop_back();
        if (!service.empty()) next += " " + service;
    }
    next += " | ";
    next += PTY_NAMES[programType];
    next += trafficProgram ? " | TP:on" : " | TP:off";
    next += trafficAnnouncement ? " TA:on" : " TA:off";
    if (next != statusText) {
        statusText = std::move(next);
        updatePending = true;
    }
}

void RDSDecoder::publishSearching() {
    const std::string next = "RDS -";
    if (next != statusText) {
        statusText = next;
        updatePending = true;
    }
}

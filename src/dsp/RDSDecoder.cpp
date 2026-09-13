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
    // Recover the 2,375-chip/s biphase clock first, then combine adjacent
    // half-bits into symbols. Two hypotheses cover unknown chip alignment.
    timingSync = symsync_crcf_create_rnyquist(
        LIQUID_FIRFILT_RRC, SAMPLES_PER_BIT / 2, 4, 1.0f, 32);
    if (timingSync) {
        symsync_crcf_set_output_rate(timingSync, 1);
        symsync_crcf_set_lf_bw(timingSync, 0.01f);
    }
    reset();
}

RDSDecoder::~RDSDecoder() {
    if (timingSync) symsync_crcf_destroy(timingSync);
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
    adaptivePhases = {};
    sampleDelay = {};
    matchedReal = 0.0;
    matchedImag = 0.0;
    sampleDelayFill = 0;
    timingOutput.clear();
    previousAdaptiveChip = {};
    adaptiveChipIndex = 0;
    havePreviousAdaptiveChip = false;
    if (timingSync) symsync_crcf_reset(timingSync);
    directBlocks = {};
    sampleIndex = 0;
    lastValidGroupSample = 0;
}

void RDSDecoder::process(const liquid_float_complex *samples, size_t count) {
    static_assert((SAMPLES_PER_BIT & (SAMPLES_PER_BIT - 1)) == 0,
                  "RDS samples per bit must be a power of two");
    if (!samples) return;
    for (size_t i = 0; i < count; ++i, ++sampleIndex) {
        if (sampleDelayFill < SAMPLES_PER_BIT) {
            sampleDelay[sampleDelayFill++] = samples[i];
            if (sampleDelayFill < SAMPLES_PER_BIT) continue;

            constexpr size_t halfBit = SAMPLES_PER_BIT / 2;
            for (size_t delay = 0; delay < halfBit; ++delay) {
                matchedReal += sampleDelay[delay].real;
                matchedImag += sampleDelay[delay].imag;
            }
            for (size_t delay = halfBit; delay < SAMPLES_PER_BIT; ++delay) {
                matchedReal -= sampleDelay[delay].real;
                matchedImag -= sampleDelay[delay].imag;
            }
        } else {
            constexpr size_t mask = SAMPLES_PER_BIT - 1;
            constexpr size_t halfBit = SAMPLES_PER_BIT / 2;
            const size_t oldest = sampleIndex & mask;
            const size_t midpoint = (oldest + halfBit) & mask;
            matchedReal += -sampleDelay[oldest].real +
                           2.0f * sampleDelay[midpoint].real - samples[i].real;
            matchedImag += -sampleDelay[oldest].imag +
                           2.0f * sampleDelay[midpoint].imag - samples[i].imag;
            sampleDelay[oldest] = samples[i];
        }

        // One of the 16 timing hypotheses completes on each input sample.
        const size_t phaseIndex = (sampleIndex + 1) & (SAMPLES_PER_BIT - 1);
        liquid_float_complex symbol;
        symbol.real = static_cast<float>(matchedReal);
        symbol.imag = static_cast<float>(matchedImag);
        processSymbol(phases[phaseIndex], symbol);
    }

    if (timingSync && count) {
        timingOutput.resize(count + 8);
        unsigned int chipsWritten = 0;
        symsync_crcf_execute(timingSync,
                             const_cast<liquid_float_complex *>(samples),
                             static_cast<unsigned int>(count),
                             timingOutput.data(), &chipsWritten);
        for (unsigned int i = 0; i < chipsWritten; ++i, ++adaptiveChipIndex) {
            const liquid_float_complex chip = timingOutput[i];
            if (havePreviousAdaptiveChip) {
                liquid_float_complex symbol;
                symbol.real = previousAdaptiveChip.real - chip.real;
                symbol.imag = previousAdaptiveChip.imag - chip.imag;
                const size_t pairing = (adaptiveChipIndex - 1) & 1u;
                // Tentative one-bit correction speeds acquisition while two
                // complete valid groups still guard against false lock.
                processSymbol(adaptivePhases[pairing], symbol, true);
            }
            previousAdaptiveChip = chip;
            havePreviousAdaptiveChip = true;
        }
    }
    if (synchronized && sampleIndex - lastValidGroupSample > RDS_SEARCH_TIMEOUT_SAMPLES) {
        synchronized = false;
        for (auto& phase : phases) phase.blocks.validGroups = 0;
        for (auto& phase : adaptivePhases) phase.blocks.validGroups = 0;
        publishSearching();
    }
}

void RDSDecoder::processSymbol(PhaseState& phase, liquid_float_complex symbol,
                               bool allowTentativeSync) {
    if (symbol.real * symbol.real + symbol.imag * symbol.imag < 1.0e-12f) {
        phase.havePrevious = false;
        return;
    }
    if (phase.havePrevious) {
        const float differential = symbol.real * phase.previous.real +
                                   symbol.imag * phase.previous.imag;
        processRecoveredBit(phase.blocks, differential < 0.0f,
                            allowTentativeSync);
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

void RDSDecoder::processRecoveredBit(BlockState& state, bool bit,
                                     bool allowTentativeSync) {
    constexpr uint16_t expectedSyndromes[4] = {0x0FC, 0x198, 0x168, 0x1B4};
    static const std::array<int8_t, 1024> singleBitBySyndrome = [] {
        std::array<int8_t, 1024> result;
        result.fill(-1);
        for (int bitIndex = 0; bitIndex < 26; ++bitIndex) {
            const uint16_t errorSyndrome = syndrome(1u << bitIndex);
            result[errorSyndrome] = result[errorSyndrome] == -1
                ? static_cast<int8_t>(bitIndex) : static_cast<int8_t>(-2);
        }
        return result;
    }();

    state.shift = ((state.shift << 1) | static_cast<uint32_t>(bit)) & 0x03FFFFFFu;
    if (state.expectedBlock >= 0 && --state.countdown > 0) return;

    const uint16_t currentSyndrome = syndrome(state.shift);

    if (state.expectedBlock < 0) {
        uint32_t decodedA = state.shift;
        const int damagedBit = singleBitBySyndrome[
            currentSyndrome ^ expectedSyndromes[0]];
        if (currentSyndrome == expectedSyndromes[0] ||
            (allowTentativeSync && damagedBit >= 0)) {
            if (currentSyndrome != expectedSyndromes[0]) {
                decodedA ^= 1u << damagedBit;
            }
            state.words[0] = static_cast<uint16_t>(decodedA >> 10);
            state.expectedBlock = 1;
            state.countdown = 26;
            state.groupValid = true;
        }
        return;
    }

    const int blockIndex = state.expectedBlock;
    const auto matchesExpected = [&](uint16_t value) {
        return blockIndex == 2
            ? (value == expectedSyndromes[2] || value == 0x350)
            : value == expectedSyndromes[blockIndex];
    };
    uint32_t decodedBlock = state.shift;
    bool valid = matchesExpected(currentSyndrome);
    if (!valid) {
        // CRC linearity maps an unexpected syndrome directly to its damaged
        // bit, avoiding up to 26 full polynomial divisions on noisy signals.
        const auto correctTo = [&](uint16_t expected) {
            const int bitIndex = singleBitBySyndrome[currentSyndrome ^ expected];
            if (bitIndex < 0) return false;
            decodedBlock = state.shift ^ (1u << bitIndex);
            return true;
        };
        valid = correctTo(expectedSyndromes[blockIndex]);
        if (!valid && blockIndex == 2) valid = correctTo(0x350);
    }

    if (!valid) {
        // An exact A block is stronger evidence than the current cadence.
        if (blockIndex != 0 && currentSyndrome == expectedSyndromes[0]) {
            state.words[0] = static_cast<uint16_t>(state.shift >> 10);
            state.expectedBlock = 1;
            state.countdown = 26;
            state.consecutiveErrors = 0;
            state.groupValid = true;
            return;
        }

        // Once locked, tolerate one uncorrectable block without throwing away
        // block cadence. A second consecutive miss falls back to acquisition.
        if (state.validGroups >= 2 && state.consecutiveErrors == 0) {
            state.consecutiveErrors = 1;
            state.groupValid = false;
            state.expectedBlock = (blockIndex + 1) & 3;
            state.countdown = 26;
            return;
        }

        state.expectedBlock = -1;
        state.validGroups = 0;
        state.consecutiveErrors = 0;
        state.groupValid = false;
        return;
    }

    state.consecutiveErrors = 0;
    state.words[blockIndex] = static_cast<uint16_t>(decodedBlock >> 10);
    if (blockIndex == 3) {
        if (state.groupValid) {
            state.validGroups = std::min(state.validGroups + 1, 1000);
            acceptGroup(state);
        }
        state.expectedBlock = 0;
    } else if (blockIndex == 0) {
        state.groupValid = true;
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

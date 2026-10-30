// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "RDSDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr uint64_t RDS_SEARCH_TIMEOUT_SAMPLES = 6 * 19000;
// Longest error burst corrected once a hypothesis is locked. The (26,16) code
// corrects any burst up to 5 bits, but each extra bit widens the share of
// random blocks that "correct" into valid ones.
constexpr int RDS_MAX_BURST_BITS = 5;
// Half a group: far longer than the lag between timing hypotheses decoding
// the same transmission, far shorter than the gap until it repeats.
constexpr uint64_t RDS_REPEAT_MIN_SAMPLES = 52 * RDSDecoder::SAMPLES_PER_BIT;
constexpr uint8_t RDS_TEXT_END = 0x0D;
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
    programService = {};
    clearRadioText();
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
        directBlocks.validGroups = 0;
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
    sampleIndex += SAMPLES_PER_BIT;
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
    static const std::array<uint32_t, 1024> burstBySyndrome = [] {
        // Bursts start and end with a damaged bit; the code guarantees each
        // burst of up to 5 bits has a distinct syndrome.
        std::array<uint32_t, 1024> result{};
        for (int length = 1; length <= RDS_MAX_BURST_BITS; ++length) {
            const uint32_t inner = length > 2 ? 1u << (length - 2) : 1u;
            for (uint32_t middle = 0; middle < inner; ++middle) {
                const uint32_t pattern = length == 1
                    ? 1u : 1u | (middle << 1) | (1u << (length - 1));
                for (int offset = 0; offset + length <= 26; ++offset) {
                    result[syndrome(pattern << offset)] = pattern << offset;
                }
            }
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
            state.group.words[0] = static_cast<uint16_t>(decodedA >> 10);
            state.group.corrected = decodedA != state.shift ? 1u : 0u;
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
    if (!valid && blockIndex != 0 && state.validGroups >= 2) {
        // Locked cadence makes burst correction safe enough for B-D. Block
        // B's version bit picks C or C', halving false corrections on C. PI
        // stays single-bit only: a miscorrected PI would break the lock.
        const uint16_t expected = blockIndex == 2 && (state.group.words[1] & 0x0800)
            ? 0x350 : expectedSyndromes[blockIndex];
        const uint32_t burst = burstBySyndrome[currentSyndrome ^ expected];
        if (burst) {
            decodedBlock = state.shift ^ burst;
            valid = true;
        }
    }

    if (!valid) {
        // An exact A block is stronger evidence than the current cadence.
        if (blockIndex != 0 && currentSyndrome == expectedSyndromes[0]) {
            state.group.words[0] = static_cast<uint16_t>(state.shift >> 10);
            state.group.corrected = 0;
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
    if (blockIndex == 0) state.group.corrected = 0;
    state.group.words[blockIndex] = static_cast<uint16_t>(decodedBlock >> 10);
    if (decodedBlock != state.shift) {
        state.group.corrected |= static_cast<uint8_t>(1u << blockIndex);
    }
    if (blockIndex == 3) {
        if (state.groupValid) confirmGroup(state);
        state.expectedBlock = 0;
    } else if (blockIndex == 0) {
        state.groupValid = true;
        state.expectedBlock = 1;
    } else {
        state.expectedBlock++;
    }
    state.countdown = 26;
}

void RDSDecoder::confirmGroup(BlockState& state) {
    // Across 18 timing hypotheses with error correction, noise forms a
    // valid-looking group every few minutes. Only two consecutive groups with
    // the same PI may touch shared station state; the first is replayed once
    // confirmed so its PS segment is not lost.
    const bool samePI = state.validGroups > 0 &&
                        state.group.words[0] == state.previousGroup.words[0];
    state.validGroups = samePI ? std::min(state.validGroups + 1, 1000) : 1;
    if (state.validGroups == 2) acceptGroup(state.previousGroup, 2);
    if (state.validGroups >= 2) acceptGroup(state.group, state.validGroups);
    state.previousGroup = state.group;
}

void RDSDecoder::storeCharacter(TextCell& cell, uint8_t value,
                                bool corrected) const {
    const char character = static_cast<char>(value);
    if (!corrected) {
        cell.value = character;
        cell.committed = true;
    } else if (cell.candidate == character && cell.candidateSample) {
        // A repeat within half a group is another timing hypothesis decoding
        // the same transmission, not independent evidence.
        if (sampleIndex - cell.candidateSample >= RDS_REPEAT_MIN_SAMPLES) {
            cell.value = character;
            cell.committed = true;
        }
        return;
    }
    cell.candidate = character;
    cell.candidateSample = sampleIndex;
}

void RDSDecoder::clearRadioText() {
    radioText = {};
    radioTextFlag = -1;
    radioTextDisplay.clear();
}

void RDSDecoder::acceptGroup(const Group& group, int validGroups) {
    lastValidGroupSample = sampleIndex;
    const uint16_t newProgramId = group.words[0];
    if (programId != newProgramId) {
        programId = newProgramId;
        programService = {};
        clearRadioText();
    }

    const uint16_t blockB = group.words[1];
    const bool cleanB = !(group.corrected & 0x02);
    const unsigned int groupType = (blockB >> 12) & 0x0F;
    // PTY, TP and TA repeat in nearly every group, so only exact B blocks
    // update them.
    if (cleanB) {
        trafficProgram = (blockB & 0x0400) != 0;
        programType = static_cast<uint8_t>((blockB >> 5) & 0x1F);
    }
    if (groupType == 0) {
        if (cleanB) trafficAnnouncement = (blockB & 0x0010) != 0;
        const unsigned int segment = blockB & 0x03;
        const uint16_t blockD = group.words[3];
        const bool corrected = (group.corrected & 0x0A) != 0;
        storeCharacter(programService[segment * 2],
                       static_cast<uint8_t>(blockD >> 8), corrected);
        storeCharacter(programService[segment * 2 + 1],
                       static_cast<uint8_t>(blockD & 0xFF), corrected);
    } else if (groupType == 2) {
        decodeRadioText(group);
    }
    publishStatus(validGroups);
}

void RDSDecoder::decodeRadioText(const Group& group) {
    const uint16_t blockB = group.words[1];
    const bool versionB = (blockB & 0x0800) != 0;
    const int flag = (versionB ? 2 : 0) | ((blockB >> 4) & 1);
    if (flag != radioTextFlag) {
        // The A/B flag toggles for a new message; a corrected B block may
        // have flipped it, so only an exact one may discard the text.
        if (group.corrected & 0x02) return;
        clearRadioText();
        radioTextFlag = flag;
    }

    const size_t segment = blockB & 0x0F;
    const size_t capacity = versionB ? 32 : 64;
    if (versionB) {
        const bool corrected = (group.corrected & 0x0A) != 0;
        storeCharacter(radioText[segment * 2],
                       static_cast<uint8_t>(group.words[3] >> 8), corrected);
        storeCharacter(radioText[segment * 2 + 1],
                       static_cast<uint8_t>(group.words[3] & 0xFF), corrected);
    } else {
        for (size_t i = 0; i < 4; ++i) {
            const size_t block = i < 2 ? 2 : 3;
            const bool corrected = (group.corrected & (0x02 | (1u << block))) != 0;
            const uint16_t word = group.words[block];
            storeCharacter(radioText[segment * 4 + i],
                           static_cast<uint8_t>(i % 2 ? word & 0xFF : word >> 8),
                           corrected);
        }
    }

    // Show a message only once every character up to the terminator is in.
    std::string text;
    for (size_t i = 0; i < capacity; ++i) {
        if (!radioText[i].committed) return;
        const uint8_t value = static_cast<uint8_t>(radioText[i].value);
        if (value == RDS_TEXT_END) break;
        text += value >= 32 && value <= 126 ? static_cast<char>(value) : ' ';
    }
    const size_t first = text.find_first_not_of(' ');
    text = first == std::string::npos
        ? std::string() : text.substr(first, text.find_last_not_of(' ') - first + 1);
    radioTextDisplay = std::move(text);
}

void RDSDecoder::publishStatus(int validGroups) {
    if (validGroups < 2 || !programId) return;
    synchronized = true;

    char id[5];
    std::snprintf(id, sizeof(id), "%04X", programId);
    std::string next = "RDS SYNC ";
    next += id;
    if (std::all_of(programService.begin(), programService.end(),
                    [](const TextCell& cell) { return cell.committed; })) {
        std::string service;
        for (const TextCell& cell : programService) {
            const uint8_t value = static_cast<uint8_t>(cell.value);
            service += value >= 32 && value <= 126 ? cell.value : ' ';
        }
        while (!service.empty() && service.back() == ' ') service.pop_back();
        if (!service.empty()) next += " " + service;
    }
    next += " | ";
    next += PTY_NAMES[programType];
    next += trafficProgram ? " | TP:on" : " | TP:off";
    next += trafficAnnouncement ? " TA:on" : " TA:off";
    // RadioText goes on its own line so the station label stays compact.
    if (!radioTextDisplay.empty()) next += "\n" + radioTextDisplay;
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

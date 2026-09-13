// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#pragma once

#include "liquid/liquid.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class RDSDecoder {
public:
    static constexpr size_t SAMPLES_PER_BIT = 16;

    RDSDecoder();
    ~RDSDecoder();
    RDSDecoder(const RDSDecoder&) = delete;
    RDSDecoder& operator=(const RDSDecoder&) = delete;
    void reset();
    void resetSignal();
    void process(const liquid_float_complex *samples, size_t count);
    bool takeUpdate(std::string& status);

    // Recovered-bit entry point, also useful for deterministic tests.
    void processBit(bool bit);

private:
    struct BlockState {
        uint32_t shift = 0;
        int countdown = 0;
        int expectedBlock = -1;
        int validGroups = 0;
        int consecutiveErrors = 0;
        bool groupValid = false;
        std::array<uint16_t, 4> words{};
    };
    struct PhaseState {
        liquid_float_complex previous{};
        bool havePrevious = false;
        BlockState blocks;
    };

    std::array<PhaseState, SAMPLES_PER_BIT> phases{};
    std::array<PhaseState, 2> adaptivePhases{};
    std::array<liquid_float_complex, SAMPLES_PER_BIT> sampleDelay{};
    double matchedReal = 0.0;
    double matchedImag = 0.0;
    size_t sampleDelayFill = 0;
    symsync_crcf timingSync = nullptr;
    std::vector<liquid_float_complex> timingOutput;
    liquid_float_complex previousAdaptiveChip{};
    uint64_t adaptiveChipIndex = 0;
    bool havePreviousAdaptiveChip = false;
    BlockState directBlocks;
    uint64_t sampleIndex = 0;
    uint16_t programId = 0;
    std::array<char, 8> programService{};
    uint8_t serviceSegments = 0;
    uint8_t programType = 0;
    bool trafficProgram = false;
    bool trafficAnnouncement = false;
    bool synchronized = false;
    uint64_t lastValidGroupSample = 0;
    std::string statusText;
    bool updatePending = false;

    static uint16_t syndrome(uint32_t block);
    void processSymbol(PhaseState& phase, liquid_float_complex symbol,
                       bool allowTentativeSync = false);
    void processRecoveredBit(BlockState& state, bool bit,
                             bool allowTentativeSync = false);
    void acceptGroup(BlockState& state);
    void publishStatus(int validGroups);
    void publishSearching();
};

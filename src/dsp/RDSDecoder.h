// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#pragma once

#include "liquid/liquid.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

class RDSDecoder {
public:
    static constexpr size_t SAMPLES_PER_BIT = 16;

    RDSDecoder();
    void reset();
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
        std::array<uint16_t, 4> words{};
    };
    struct PhaseState {
        liquid_float_complex first{};
        liquid_float_complex second{};
        liquid_float_complex previous{};
        bool havePrevious = false;
        bool primed = false;
        BlockState blocks;
    };

    std::array<PhaseState, SAMPLES_PER_BIT> phases{};
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
    void processSymbol(PhaseState& phase, liquid_float_complex symbol);
    void processRecoveredBit(BlockState& state, bool bit);
    void acceptGroup(BlockState& state);
    void publishStatus(int validGroups);
    void publishSearching();
};

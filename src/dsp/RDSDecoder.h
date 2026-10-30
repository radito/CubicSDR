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
    struct Group {
        std::array<uint16_t, 4> words{};
        // Bit n is set when block n needed error correction.
        uint8_t corrected = 0;
    };
    struct BlockState {
        uint32_t shift = 0;
        int countdown = 0;
        int expectedBlock = -1;
        int validGroups = 0;
        int consecutiveErrors = 0;
        bool groupValid = false;
        Group group;
        Group previousGroup;
    };
    // A character from a corrected block is shown only once a later
    // transmission repeats it.
    struct TextCell {
        char value = ' ';
        char candidate = 0;
        uint64_t candidateSample = 0;
        bool committed = false;
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
    std::array<TextCell, 8> programService{};
    std::array<TextCell, 64> radioText{};
    int radioTextFlag = -1;
    std::string radioTextDisplay;
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
    void confirmGroup(BlockState& state);
    void acceptGroup(const Group& group, int validGroups);
    void decodeRadioText(const Group& group);
    void clearRadioText();
    void storeCharacter(TextCell& cell, uint8_t value, bool corrected) const;
    void publishStatus(int validGroups);
    void publishSearching();
};

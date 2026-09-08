// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "RDSDecoder.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

uint16_t blockSyndrome(uint32_t block) {
    constexpr uint32_t polynomial = 0x5B9;
    for (int bit = 25; bit >= 10; --bit) {
        if (block & (1u << bit)) block ^= polynomial << (bit - 10);
    }
    return static_cast<uint16_t>(block & 0x3FFu);
}

uint32_t makeBlock(uint16_t data, uint16_t expectedSyndrome) {
    const uint32_t payload = static_cast<uint32_t>(data) << 10;
    for (uint32_t check = 0; check < 1024; ++check) {
        if (blockSyndrome(payload | check) == expectedSyndrome) return payload | check;
    }
    return 0;
}

void feedBlock(RDSDecoder& decoder, uint32_t block) {
    for (int bit = 25; bit >= 0; --bit) decoder.processBit((block >> bit) & 1u);
}

void feedGroup(RDSDecoder& decoder, uint16_t pi, uint16_t blockB, uint16_t blockD) {
    constexpr std::array<uint16_t, 4> offsets{{0x0FC, 0x198, 0x168, 0x1B4}};
    const std::array<uint16_t, 4> words{{pi, blockB, 0, blockD}};
    for (size_t i = 0; i < words.size(); ++i) {
        feedBlock(decoder, makeBlock(words[i], offsets[i]));
    }
}

void appendEncodedBlock(std::vector<liquid_float_complex>& samples,
                        uint32_t block, float& differentialState) {
    for (int bit = 25; bit >= 0; --bit) {
        if ((block >> bit) & 1u) differentialState = -differentialState;
        for (size_t sample = 0; sample < RDSDecoder::SAMPLES_PER_BIT; ++sample) {
            liquid_float_complex value;
            value.real = sample < RDSDecoder::SAMPLES_PER_BIT / 2
                ? differentialState : -differentialState;
            value.imag = 0.25f * value.real;
            samples.push_back(value);
        }
    }
}

} // namespace

CUBIC_TEST(rds_decoder_extracts_pi_and_program_service) {
    RDSDecoder decoder;
    constexpr uint16_t pi = 0xA123;
    constexpr char name[9] = "TEST FM ";

    for (uint16_t segment = 0; segment < 4; ++segment) {
        const uint16_t characters =
            (static_cast<uint16_t>(name[segment * 2]) << 8) |
            static_cast<uint8_t>(name[segment * 2 + 1]);
        const uint16_t blockB = 0x0400 | (10u << 5) | 0x0010 | segment;
        feedGroup(decoder, pi, blockB, characters);
    }

    std::string status;
    CUBIC_REQUIRE(decoder.takeUpdate(status));
    CUBIC_REQUIRE(status == "RDS SYNC A123 TEST FM | Pop Music | TP:on TA:on");
    CUBIC_REQUIRE(!decoder.takeUpdate(status));

    decoder.reset();
    CUBIC_REQUIRE(decoder.takeUpdate(status));
    CUBIC_REQUIRE(status == "RDS -");
}

CUBIC_TEST(rds_decoder_recovers_streamed_biphase_samples) {
    constexpr std::array<uint16_t, 4> offsets{{0x0FC, 0x198, 0x168, 0x1B4}};
    constexpr uint16_t pi = 0xBEEF;
    constexpr char name[9] = "RADIO123";
    std::vector<liquid_float_complex> samples;
    float differentialState = 1.0f;

    // One priming symbol supplies the previous differential state.
    appendEncodedBlock(samples, 0, differentialState);
    samples.erase(samples.begin() + RDSDecoder::SAMPLES_PER_BIT, samples.end());
    for (uint16_t segment = 0; segment < 4; ++segment) {
        const uint16_t words[4] = {
            pi,
            static_cast<uint16_t>(0x0400 | (10u << 5) | 0x0010 | segment),
            0,
            static_cast<uint16_t>((static_cast<uint16_t>(name[segment * 2]) << 8) |
                                  static_cast<uint8_t>(name[segment * 2 + 1]))
        };
        for (size_t block = 0; block < 4; ++block) {
            appendEncodedBlock(samples, makeBlock(words[block], offsets[block]),
                               differentialState);
        }
    }

    RDSDecoder decoder;
    const size_t split = samples.size() / 3;
    decoder.process(samples.data(), split);
    decoder.process(samples.data() + split, samples.size() - split);

    std::string status;
    CUBIC_REQUIRE(decoder.takeUpdate(status));
    CUBIC_REQUIRE(status == "RDS SYNC BEEF RADIO123 | Pop Music | TP:on TA:on");

    std::vector<liquid_float_complex> silence(
        3 * 19000 + RDSDecoder::SAMPLES_PER_BIT + 1);
    decoder.process(silence.data(), silence.size());
    CUBIC_REQUIRE(decoder.takeUpdate(status));
    CUBIC_REQUIRE(status == "RDS -");
}

CUBIC_TEST(rds_decoder_rejects_a_corrupt_group) {
    RDSDecoder decoder;
    std::string status;
    CUBIC_REQUIRE(decoder.takeUpdate(status));
    feedBlock(decoder, makeBlock(0x1234, 0x0FC));
    feedBlock(decoder, makeBlock(0x0000, 0x198) ^ (1u << 12));
    feedBlock(decoder, makeBlock(0x0000, 0x168));
    feedBlock(decoder, makeBlock(0x4142, 0x1B4));

    CUBIC_REQUIRE(!decoder.takeUpdate(status));
}

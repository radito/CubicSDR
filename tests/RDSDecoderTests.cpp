// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "RDSDecoder.h"
#include "ModemFMStereo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
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

// Feeds one group; block C uses the C' offset for version B groups.
void feedGroupWords(RDSDecoder& decoder, const std::array<uint16_t, 4>& words,
                    const std::array<uint32_t, 4>& errors = {}) {
    const bool versionB = (words[1] & 0x0800) != 0;
    const std::array<uint16_t, 4> offsets{{0x0FC, 0x198,
        static_cast<uint16_t>(versionB ? 0x350 : 0x168), 0x1B4}};
    for (size_t i = 0; i < words.size(); ++i) {
        feedBlock(decoder, makeBlock(words[i], offsets[i]) ^ errors[i]);
    }
}

uint16_t characterPair(const char *text, size_t index) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(static_cast<uint8_t>(text[index])) << 8) |
        static_cast<uint8_t>(text[index + 1]));
}

void feedServiceName(RDSDecoder& decoder, uint16_t pi, const char *name) {
    for (uint16_t segment = 0; segment < 4; ++segment) {
        feedGroupWords(decoder, {{pi, static_cast<uint16_t>((10u << 5) | segment),
                                  0, characterPair(name, segment * 2)}});
    }
}

// Sends a 2A RadioText message, padded to whole four-character segments.
void feedRadioText2A(RDSDecoder& decoder, uint16_t pi, const std::string& text,
                     bool textFlag) {
    std::string padded = text;
    while (padded.size() % 4) padded += ' ';
    for (size_t segment = 0; segment < padded.size() / 4; ++segment) {
        const uint16_t blockB = static_cast<uint16_t>(
            0x2000 | (10u << 5) | (textFlag ? 0x10 : 0) | segment);
        feedGroupWords(decoder, {{pi, blockB, characterPair(padded.c_str(), segment * 4),
                                  characterPair(padded.c_str(), segment * 4 + 2)}});
    }
}

std::string lastStatus(RDSDecoder& decoder) {
    std::string status;
    std::string latest;
    while (decoder.takeUpdate(status)) latest = status;
    return latest;
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

void appendBlockBits(std::vector<bool>& bits, uint32_t block) {
    for (int bit = 25; bit >= 0; --bit) bits.push_back((block >> bit) & 1u);
}

std::vector<liquid_float_complex> makeClockDriftSamples(
        const std::vector<bool>& bits) {
    std::vector<liquid_float_complex> samples;
    float differentialState = 1.0f;
    size_t outputIndex = 0;
    for (size_t bitIndex = 0; bitIndex < bits.size(); ++bitIndex) {
        if (bits[bitIndex]) differentialState = -differentialState;
        // About +0.9% clock error, large enough to cross timing phases during
        // a short test while remaining inside the synchronizer pull range.
        const size_t symbolSamples = bitIndex % 7 == 0 ? 17 : 16;
        for (size_t sample = 0; sample < symbolSamples; ++sample, ++outputIndex) {
            const float symbol = sample * 2 < symbolSamples
                ? differentialState : -differentialState;
            const float noise = 0.08f * std::sin(0.37f * float(outputIndex));
            liquid_float_complex value;
            value.real = symbol + noise;
            value.imag = 0.3f * symbol - 0.5f * noise;
            samples.push_back(value);
        }
    }
    return samples;
}

struct BroadcastOptions {
    bool stereoPilot = true;
    float snrDb = 25.0f;
    // Without a pilot, the RDS subcarrier is only specified to 57 kHz +/-6 Hz.
    float monoCarrierOffsetHz = 3.0f;
    unsigned int seed = 1;
    size_t groups = 12;
    // When set, every other group is a 2A RadioText group carrying it.
    std::string radioText;
};

// FM-modulates a broadcast multiplex carrying PS "MONO FM " under PI 0x5A5A,
// runs it through the FMS demodulator, and returns the last RDS status.
std::string demodulateBroadcastRds(const BroadcastOptions& options) {
    constexpr std::array<uint16_t, 4> offsets{{0x0FC, 0x198, 0x168, 0x1B4}};
    constexpr char name[9] = "MONO FM ";
    constexpr long long sampleRate = 200'000;
    constexpr double twoPi = 2.0 * M_PI;

    std::string radioText = options.radioText;
    while (radioText.size() % 4) radioText += ' ';
    const size_t textSegments = radioText.size() / 4;

    std::vector<bool> bits;
    size_t serviceGroup = 0;
    size_t textGroup = 0;
    for (size_t group = 0; group < options.groups; ++group) {
        uint16_t words[4];
        if (textSegments && group % 2) {
            const size_t segment = textGroup++ % textSegments;
            words[0] = 0x5A5A;
            words[1] = static_cast<uint16_t>(0x2000 | (10u << 5) | segment);
            words[2] = characterPair(radioText.c_str(), segment * 4);
            words[3] = characterPair(radioText.c_str(), segment * 4 + 2);
        } else {
            const uint16_t segment = serviceGroup++ & 3u;
            words[0] = 0x5A5A;
            words[1] = static_cast<uint16_t>((10u << 5) | segment);
            words[2] = 0;
            words[3] = characterPair(name, segment * 2);
        }
        for (size_t block = 0; block < 4; ++block) {
            appendBlockBits(bits, makeBlock(words[block], offsets[block]));
        }
    }

    ModemFMStereo modem;
    auto *kit = modem.buildKit(sampleRate, 48'000);
    std::mt19937 rng(options.seed);
    std::normal_distribution<float> gaussian(0.0f, 1.0f);
    const float noiseAmplitude =
        std::pow(10.0f, -options.snrDb / 20.0f) / std::sqrt(2.0f);
    const size_t totalSamples = static_cast<size_t>(
        double(bits.size()) / 1187.5 * double(sampleRate));
    double fmPhase = 0.0;
    float differentialState = 1.0f;
    size_t currentBit = SIZE_MAX;
    std::string status;
    std::string latest;

    for (size_t start = 0; start < totalSamples; start += 4096) {
        ModemIQData input;
        input.sampleRate = sampleRate;
        input.data.resize(std::min<size_t>(4096, totalSamples - start));
        for (size_t i = 0; i < input.data.size(); ++i) {
            const double t = double(start + i) / double(sampleRate);
            const double bitPosition = t * 1187.5;
            const size_t bitIndex = static_cast<size_t>(bitPosition);
            if (bitIndex != currentBit) {
                currentBit = bitIndex;
                if (bits[bitIndex]) differentialState = -differentialState;
            }
            const float biphase = bitPosition - double(bitIndex) < 0.5
                ? differentialState : -differentialState;
            const double pilotPhase = twoPi * 19'000.0 * t;

            double mpx = 0.4 * std::sin(twoPi * 1'000.0 * t) +
                         0.3 * std::sin(twoPi * 5'300.0 * t);
            if (options.stereoPilot) {
                mpx += 0.09 * std::sin(pilotPhase) +
                       0.25 * std::sin(twoPi * 2'000.0 * t) * std::sin(2.0 * pilotPhase);
                mpx += 0.04 * biphase * std::cos(3.0 * pilotPhase);
            } else {
                mpx += 0.04 * biphase * std::cos(
                    twoPi * (57'000.0 + options.monoCarrierOffsetHz) * t + 0.7);
            }

            fmPhase += twoPi * 75'000.0 / double(sampleRate) * mpx;
            input.data[i].real = float(std::cos(fmPhase)) + noiseAmplitude * gaussian(rng);
            input.data[i].imag = float(std::sin(fmPhase)) + noiseAmplitude * gaussian(rng);
        }
        AudioThreadInput audio;
        modem.demodulate(kit, &input, &audio);
        while (modem.takeStatus(status)) latest = status;
    }
    modem.disposeKit(kit);
    return latest;
}

} // namespace

CUBIC_TEST(fms_demodulator_decodes_rds_with_stereo_pilot) {
    const std::string status = demodulateBroadcastRds({});
    CUBIC_REQUIRE(status == "RDS SYNC 5A5A MONO FM | Pop Music | TP:off TA:off");
}

CUBIC_TEST(fms_demodulator_decodes_radiotext) {
    BroadcastOptions options;
    options.groups = 20;
    options.radioText = "Hello from RDS\r";
    const std::string status = demodulateBroadcastRds(options);
    CUBIC_REQUIRE(status ==
                  "RDS SYNC 5A5A MONO FM | Pop Music | TP:off TA:off\nHello from RDS");
}

CUBIC_TEST(fms_demodulator_decodes_rds_from_mono_station_without_pilot) {
    BroadcastOptions options;
    options.stereoPilot = false;
    const std::string status = demodulateBroadcastRds(options);
    CUBIC_REQUIRE(status == "RDS SYNC 5A5A MONO FM | Pop Music | TP:off TA:off");
}

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

    decoder.resetSignal();
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
        6 * 19000 + RDSDecoder::SAMPLES_PER_BIT + 1);
    decoder.process(silence.data(), silence.size());
    CUBIC_REQUIRE(decoder.takeUpdate(status));
    CUBIC_REQUIRE(status == "RDS -");
}

CUBIC_TEST(rds_decoder_acquires_with_clock_drift_and_noise) {
    constexpr std::array<uint16_t, 4> offsets{{0x0FC, 0x198, 0x168, 0x1B4}};
    std::vector<bool> bits{false}; // Prime differential decoding.
    for (uint16_t group = 0; group < 8; ++group) {
        const uint16_t segment = group & 3u;
        const uint16_t words[4] = {
            0xCAFE,
            static_cast<uint16_t>((10u << 5) | segment),
            0,
            static_cast<uint16_t>(0x4141u + segment)
        };
        for (size_t block = 0; block < 4; ++block) {
            appendBlockBits(bits, makeBlock(words[block], offsets[block]));
        }
    }
    const auto samples = makeClockDriftSamples(bits);

    RDSDecoder decoder;
    for (size_t offset = 0; offset < samples.size();) {
        const size_t count = std::min<size_t>(137, samples.size() - offset);
        decoder.process(samples.data() + offset, count);
        offset += count;
    }

    std::string status;
    CUBIC_REQUIRE(decoder.takeUpdate(status));
    CUBIC_REQUIRE(status.find("RDS SYNC CAFE") == 0);
}

CUBIC_TEST(rds_decoder_corrects_a_single_corrupt_bit) {
    for (int damagedBit = 0; damagedBit < 26; ++damagedBit) {
        RDSDecoder decoder;
        std::string status;
        CUBIC_REQUIRE(decoder.takeUpdate(status));
        for (int group = 0; group < 2; ++group) {
            feedBlock(decoder, makeBlock(0x1234, 0x0FC));
            feedBlock(decoder, makeBlock(0x0000, 0x198) ^ (1u << damagedBit));
            feedBlock(decoder, makeBlock(0x0000, 0x168));
            feedBlock(decoder, makeBlock(0x4142, 0x1B4));
        }

        CUBIC_REQUIRE(decoder.takeUpdate(status));
        CUBIC_REQUIRE(status == "RDS SYNC 1234 | None | TP:off TA:off");
    }
}

CUBIC_TEST(rds_decoder_retains_cadence_after_one_bad_block) {
    RDSDecoder decoder;
    std::string status;
    CUBIC_REQUIRE(decoder.takeUpdate(status));

    for (int group = 0; group < 2; ++group) {
        feedGroup(decoder, 0x1234, 0x0000, 0x4142);
    }
    CUBIC_REQUIRE(decoder.takeUpdate(status));

    const uint32_t damagedB = makeBlock(0x0000, 0x198) ^ (1u << 3) ^ (1u << 12);
    for (int bit = 0; bit < 26; ++bit) {
        CUBIC_REQUIRE(blockSyndrome(damagedB ^ (1u << bit)) != 0x198);
    }
    feedBlock(decoder, makeBlock(0x1234, 0x0FC));
    feedBlock(decoder, damagedB);
    feedBlock(decoder, makeBlock(0x0000, 0x168));
    feedBlock(decoder, makeBlock(0x4142, 0x1B4));

    // The next clean group is accepted immediately because cadence survived.
    feedGroup(decoder, 0x1234, static_cast<uint16_t>(10u << 5), 0x4142);
    CUBIC_REQUIRE(decoder.takeUpdate(status));
    CUBIC_REQUIRE(status == "RDS SYNC 1234 | Pop Music | TP:off TA:off");
}

CUBIC_TEST(rds_decoder_drops_cadence_after_two_bad_blocks) {
    RDSDecoder decoder;
    std::string status;
    CUBIC_REQUIRE(decoder.takeUpdate(status));
    for (int group = 0; group < 2; ++group) {
        feedGroup(decoder, 0x1234, 0x0000, 0x4142);
    }
    CUBIC_REQUIRE(decoder.takeUpdate(status));

    feedBlock(decoder, makeBlock(0x1234, 0x0FC));
    feedBlock(decoder, makeBlock(0x0000, 0x198) ^ (1u << 3) ^ (1u << 12));
    feedBlock(decoder, makeBlock(0x0000, 0x168) ^ (1u << 4) ^ (1u << 13));
    feedBlock(decoder, makeBlock(0x4142, 0x1B4));

    feedGroup(decoder, 0x1234, static_cast<uint16_t>(10u << 5), 0x4142);
    CUBIC_REQUIRE(!decoder.takeUpdate(status));
    feedGroup(decoder, 0x1234, static_cast<uint16_t>(10u << 5), 0x4142);

    CUBIC_REQUIRE(decoder.takeUpdate(status));
    CUBIC_REQUIRE(status == "RDS SYNC 1234 | Pop Music | TP:off TA:off");
}

CUBIC_TEST(rds_decoder_ignores_unconfirmed_foreign_group) {
    RDSDecoder decoder;
    std::string status;
    CUBIC_REQUIRE(decoder.takeUpdate(status));
    for (int group = 0; group < 2; ++group) {
        feedGroup(decoder, 0x1234, static_cast<uint16_t>(10u << 5), 0x4142);
    }
    CUBIC_REQUIRE(decoder.takeUpdate(status));
    CUBIC_REQUIRE(status == "RDS SYNC 1234 | Pop Music | TP:off TA:off");

    // A lone group with another PI, as noise can produce, must not replace
    // the locked station or its PTY.
    feedGroup(decoder, 0x9999, static_cast<uint16_t>(1u << 5), 0x4142);
    CUBIC_REQUIRE(!decoder.takeUpdate(status));
    feedGroup(decoder, 0x1234, static_cast<uint16_t>(10u << 5), 0x4142);
    feedGroup(decoder, 0x1234, static_cast<uint16_t>(10u << 5), 0x4142);
    CUBIC_REQUIRE(!decoder.takeUpdate(status));

    // A genuine PI change is accepted once two groups confirm it.
    feedGroup(decoder, 0x9999, static_cast<uint16_t>(1u << 5), 0x4142);
    CUBIC_REQUIRE(!decoder.takeUpdate(status));
    feedGroup(decoder, 0x9999, static_cast<uint16_t>(1u << 5), 0x4142);
    CUBIC_REQUIRE(decoder.takeUpdate(status));
    CUBIC_REQUIRE(status == "RDS SYNC 9999 | News | TP:off TA:off");
}

CUBIC_TEST(rds_decoder_shows_radiotext_on_its_own_line) {
    RDSDecoder decoder;
    feedServiceName(decoder, 0x1234, "NEWS FM ");
    feedRadioText2A(decoder, 0x1234, "Now playing: Test Song\r", false);
    CUBIC_REQUIRE(lastStatus(decoder) ==
                  "RDS SYNC 1234 NEWS FM | Pop Music | TP:off TA:off\n"
                  "Now playing: Test Song");

    // Toggling the A/B flag announces a new message: the old one is dropped
    // and the new one appears once complete.
    feedRadioText2A(decoder, 0x1234, "Next up: Another\r", true);
    CUBIC_REQUIRE(lastStatus(decoder) ==
                  "RDS SYNC 1234 NEWS FM | Pop Music | TP:off TA:off\n"
                  "Next up: Another");
}

CUBIC_TEST(rds_decoder_waits_for_complete_radiotext) {
    RDSDecoder decoder;
    feedServiceName(decoder, 0x1234, "NEWS FM ");
    lastStatus(decoder);
    const char text[] = "Partial message\r";
    const auto feedSegment = [&](uint16_t segment) {
        feedGroupWords(decoder, {{0x1234, static_cast<uint16_t>(0x2000 | (10u << 5) | segment),
                                  characterPair(text, segment * 4),
                                  characterPair(text, segment * 4 + 2)}});
    };
    // Segment 1 of four is missing, so nothing is shown yet.
    for (uint16_t segment : {0, 2, 3}) feedSegment(segment);
    CUBIC_REQUIRE(lastStatus(decoder).find('\n') == std::string::npos);
    feedSegment(1);
    CUBIC_REQUIRE(lastStatus(decoder) ==
                  "RDS SYNC 1234 NEWS FM | Pop Music | TP:off TA:off\nPartial message");
}

CUBIC_TEST(rds_decoder_reads_version_b_radiotext) {
    RDSDecoder decoder;
    feedServiceName(decoder, 0x1234, "NEWS FM ");
    const char text[] = "Short 2B\r ";
    for (uint16_t segment = 0; segment < 5; ++segment) {
        feedGroupWords(decoder, {{0x1234, static_cast<uint16_t>(0x2800 | (10u << 5) | segment),
                                  0x1234, characterPair(text, segment * 2)}});
    }
    CUBIC_REQUIRE(lastStatus(decoder) ==
                  "RDS SYNC 1234 NEWS FM | Pop Music | TP:off TA:off\nShort 2B");
}

CUBIC_TEST(rds_decoder_corrects_bursts_only_once_locked) {
    constexpr uint32_t burst = 0x1Du << 7; // 5-bit burst 11101
    // Before lock, a burst-damaged block is rejected and acquisition restarts.
    RDSDecoder unlocked;
    feedGroupWords(unlocked, {{0x1234, 0x0000, 0, 0x4142}}, {{0, burst, 0, 0}});
    feedGroupWords(unlocked, {{0x1234, 0x0000, 0, 0x4142}});
    CUBIC_REQUIRE(lastStatus(unlocked) == "RDS -");

    // Once locked, the same burst is corrected and the group is used.
    RDSDecoder locked;
    feedServiceName(locked, 0x1234, "ABCDEFGH");
    CUBIC_REQUIRE(lastStatus(locked) == "RDS SYNC 1234 ABCDEFGH | Pop Music | TP:off TA:off");
    feedGroupWords(locked, {{0x1234, static_cast<uint16_t>((10u << 5) | 0x10), 0,
                             characterPair("ABCDEFGH", 0)}}, {{0, 0, 0, burst}});
    CUBIC_REQUIRE(lastStatus(locked) == "RDS SYNC 1234 ABCDEFGH | Pop Music | TP:off TA:on");
}

CUBIC_TEST(rds_decoder_confirms_corrected_characters_before_display) {
    constexpr uint32_t burst = 0x0Bu << 3; // 4-bit burst 1011
    RDSDecoder decoder;
    feedGroupWords(decoder, {{0x1234, static_cast<uint16_t>(10u << 5), 0, characterPair("ABCDEFGH", 0)}});
    feedGroupWords(decoder, {{0x1234, static_cast<uint16_t>((10u << 5) | 1), 0, characterPair("ABCDEFGH", 2)}});
    feedGroupWords(decoder, {{0x1234, static_cast<uint16_t>((10u << 5) | 2), 0, characterPair("ABCDEFGH", 4)}});
    // The last segment arrives only through a corrected block: not yet shown.
    feedGroupWords(decoder, {{0x1234, static_cast<uint16_t>((10u << 5) | 3), 0, characterPair("ABCDEFGH", 6)}},
                   {{0, 0, 0, burst}});
    CUBIC_REQUIRE(lastStatus(decoder) == "RDS SYNC 1234 | Pop Music | TP:off TA:off");
    // A later, independently corrected repeat confirms it.
    feedGroupWords(decoder, {{0x1234, static_cast<uint16_t>((10u << 5) | 3), 0, characterPair("ABCDEFGH", 6)}},
                   {{0, 0, 0, burst << 9}});
    CUBIC_REQUIRE(lastStatus(decoder) == "RDS SYNC 1234 ABCDEFGH | Pop Music | TP:off TA:off");
}

CUBIC_TEST(rds_decoder_keeps_pty_from_corrected_b_blocks_out) {
    RDSDecoder decoder;
    feedServiceName(decoder, 0x1234, "ABCDEFGH");
    lastStatus(decoder);
    // A corrected B block claiming PTY News must not override Pop Music.
    feedGroupWords(decoder, {{0x1234, static_cast<uint16_t>(1u << 5), 0, characterPair("ABCDEFGH", 0)}},
                   {{0, 0x7u << 12, 0, 0}});
    CUBIC_REQUIRE(lastStatus(decoder).empty());
}

CUBIC_TEST(rds_decoder_does_not_confirm_a_correction_across_timing_hypotheses) {
    // Every timing hypothesis decodes the same corrupted transmission. Their
    // agreement must not count as the repeat a corrected character needs.
    constexpr std::array<uint16_t, 4> offsets{{0x0FC, 0x198, 0x168, 0x1B4}};
    constexpr char name[9] = "RADIO123";
    std::vector<liquid_float_complex> samples;
    float differentialState = 1.0f;
    appendEncodedBlock(samples, 0, differentialState);
    samples.erase(samples.begin() + RDSDecoder::SAMPLES_PER_BIT, samples.end());
    const auto appendGroup = [&](uint16_t segment, uint32_t damageD) {
        const uint16_t words[4] = {
            0xBEEF, static_cast<uint16_t>((10u << 5) | segment), 0,
            characterPair(name, segment * 2)};
        for (size_t block = 0; block < 4; ++block) {
            appendEncodedBlock(samples, makeBlock(words[block], offsets[block]) ^
                               (block == 3 ? damageD : 0u), differentialState);
        }
    };
    for (uint16_t segment = 0; segment < 3; ++segment) appendGroup(segment, 0);
    appendGroup(3, 0x0Bu << 3);

    RDSDecoder decoder;
    decoder.process(samples.data(), samples.size());
    CUBIC_REQUIRE(lastStatus(decoder) == "RDS SYNC BEEF | Pop Music | TP:off TA:off");

    samples.clear();
    appendGroup(3, 0);
    decoder.process(samples.data(), samples.size());
    CUBIC_REQUIRE(lastStatus(decoder) == "RDS SYNC BEEF RADIO123 | Pop Music | TP:off TA:off");
}

CUBIC_TEST(rds_decoder_ignores_noise_after_lock) {
    // Burst correction "repairs" about a third of random blocks once locked;
    // the PI gate and repeat confirmation must keep that out of the display.
    RDSDecoder decoder;
    feedServiceName(decoder, 0x1234, "ABCDEFGH");
    feedRadioText2A(decoder, 0x1234, "Real text\r", false);
    CUBIC_REQUIRE(lastStatus(decoder) ==
                  "RDS SYNC 1234 ABCDEFGH | Pop Music | TP:off TA:off\nReal text");
    std::mt19937 rng(7);
    std::string status;
    for (int bit = 0; bit < 200'000; ++bit) {
        decoder.processBit(rng() & 1u);
        CUBIC_REQUIRE(!decoder.takeUpdate(status));
    }
}

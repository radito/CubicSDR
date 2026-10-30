// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class SquelchDetector {
public:
    float update(float levelDb, double blockSeconds, bool discontinuity = false);
    void reset();

    // Whether a closed squelch opens on this block. Quality is only
    // consulted when the modem provides one.
    static bool shouldOpen(float levelDb, float closeLevelDb,
                           bool hasQuality, float quality);
    // 0 while an open squelch is held. Otherwise, how long after it was last
    // held it may stay open: short for noise so the tail is cut, long for a
    // level dip on clean audio so fading signals are not chopped.
    static std::int64_t closeHangMilliseconds(float levelDb, float closeLevelDb,
                                              bool hasQuality, float quality);

private:
    bool initialized = false;
    float filteredLevelDb = -100.0f;
};

class NBFMNoiseDetector {
public:
    bool process(const std::vector<float>& discriminator,
                 int sampleRate,
                 bool discontinuity,
                 float& qualityOut);
    void reset();

private:
    struct Biquad {
        double b0 = 0.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        double process(double sample);
        void clearState();
    };

    bool configure(int sampleRate);
    double highPass(double sample);
    void clearFilterState();

    int configuredSampleRate = 0;
    bool initialized = false;
    float filteredQuality = 0.0f;
    // 4th-order Butterworth high-pass isolating the band above voice.
    std::array<Biquad, 2> sections{};
    // High-band power of an open channel, whose discriminator output is
    // white with variance 1/3 for freqdem kf = 0.5.
    double openChannelNoisePower = 0.0;
};

enum class SquelchAudioResult {
    Suppress,
    PassThrough,
    Processed
};

class SquelchAudioGate {
public:
    SquelchAudioResult process(const std::vector<float>& input,
                               int sampleRate,
                               int channels,
                               bool enabled,
                               bool open,
                               bool discontinuity,
                               std::vector<float>& output);
    void reset();

private:
    void configure(int sampleRate, int channels);
    void appendHistory(const std::vector<float>& input);
    void copyHistory(std::vector<float>& output) const;
    static void fadeIn(std::vector<float>& samples, int sampleRate, int channels);
    static void fadeOut(std::vector<float>& samples, int sampleRate, int channels);

    std::vector<float> history;
    std::size_t historyWrite = 0;
    std::size_t historyCount = 0;
    int configuredSampleRate = 0;
    int configuredChannels = 0;
    bool wasOpen = false;
};

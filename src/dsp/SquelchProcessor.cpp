// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "SquelchProcessor.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kAttackSeconds = 0.020;
constexpr double kReleaseSeconds = 0.100;
constexpr double kPreRollSeconds = 0.050;
constexpr double kFadeSeconds = 0.008;
constexpr float kOpenHysteresisDb = 2.0f;
constexpr std::int64_t kLevelCloseHangMilliseconds = 500;
constexpr std::int64_t kNoiseCloseHangMilliseconds = 60;
constexpr float kQualityOpen = 0.50f;
constexpr float kQualityHold = 0.25f;

// The noise band starts above voice (NBFM transmitters cut audio at 3 kHz)
// and must fit below Nyquist, which sets the lowest usable sample rate.
constexpr double kNBFMNoiseCutoffHz = 3000.0;
constexpr double kNBFMNoiseCutoffFraction = 0.28;
constexpr double kNBFMMaxCutoffFraction = 0.45;
// High-band noise power relative to an open channel. Readable signals sit
// near 0.1 even at 8 dB SNR; open channels average 1.
constexpr double kNBFMCleanNoise = 0.15;
constexpr double kNBFMNoisyNoise = 0.60;
// Rising noise lowers quality quickly so carrier drops close fast; recovery
// is slower so one clean block amid noise does not reopen.
constexpr double kNBFMQualityFallSeconds = 0.015;
constexpr double kNBFMQualityRiseSeconds = 0.050;
}

float SquelchDetector::update(float levelDb, double blockSeconds, bool discontinuity) {
    if (!std::isfinite(levelDb)) {
        return filteredLevelDb;
    }
    if (discontinuity || !initialized || blockSeconds <= 0.0) {
        initialized = true;
        filteredLevelDb = levelDb;
        return filteredLevelDb;
    }

    const double timeConstant = levelDb > filteredLevelDb ? kAttackSeconds : kReleaseSeconds;
    const double alpha = 1.0 - std::exp(-std::min(blockSeconds, 1.0) / timeConstant);
    filteredLevelDb += static_cast<float>((levelDb - filteredLevelDb) * alpha);
    return filteredLevelDb;
}

void SquelchDetector::reset() {
    initialized = false;
    filteredLevelDb = -100.0f;
}

bool SquelchDetector::shouldOpen(float levelDb, float closeLevelDb,
                                 bool hasQuality, float quality) {
    return levelDb >= closeLevelDb + kOpenHysteresisDb &&
           (!hasQuality || quality >= kQualityOpen);
}

std::int64_t SquelchDetector::closeHangMilliseconds(float levelDb, float closeLevelDb,
                                                    bool hasQuality, float quality) {
    if (hasQuality && quality < kQualityHold) {
        return kNoiseCloseHangMilliseconds;
    }
    if (levelDb < closeLevelDb) {
        return kLevelCloseHangMilliseconds;
    }
    return 0;
}

double NBFMNoiseDetector::Biquad::process(double sample) {
    const double output = b0 * sample + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = sample;
    y2 = y1;
    y1 = output;
    return output;
}

void NBFMNoiseDetector::Biquad::clearState() {
    x1 = x2 = y1 = y2 = 0.0;
}

double NBFMNoiseDetector::highPass(double sample) {
    for (Biquad& section : sections) sample = section.process(sample);
    return sample;
}

void NBFMNoiseDetector::clearFilterState() {
    for (Biquad& section : sections) section.clearState();
}

void NBFMNoiseDetector::reset() {
    configuredSampleRate = 0;
    initialized = false;
    filteredQuality = 0.0f;
    sections = {};
    openChannelNoisePower = 0.0;
}

bool NBFMNoiseDetector::configure(int sampleRate) {
    const double cutoff = std::max(kNBFMNoiseCutoffHz,
                                   kNBFMNoiseCutoffFraction * sampleRate);
    if (sampleRate <= 0 || cutoff > kNBFMMaxCutoffFraction * sampleRate) {
        reset();
        return false;
    }
    if (sampleRate == configuredSampleRate) {
        return true;
    }

    configuredSampleRate = sampleRate;
    initialized = false;
    filteredQuality = 0.0f;

    constexpr double pi = 3.14159265358979323846;
    // Pole quality factors of a 4th-order Butterworth response.
    constexpr double qualityFactors[2] = {0.54119610014619698, 1.3065629648763766};
    const double omega = 2.0 * pi * cutoff / sampleRate;
    const double cosine = std::cos(omega);
    for (size_t i = 0; i < sections.size(); ++i) {
        const double alpha = std::sin(omega) / (2.0 * qualityFactors[i]);
        const double a0 = 1.0 + alpha;
        Biquad& section = sections[i];
        section.b0 = ((1.0 + cosine) * 0.5) / a0;
        section.b1 = -(1.0 + cosine) / a0;
        section.b2 = section.b0;
        section.a1 = (-2.0 * cosine) / a0;
        section.a2 = (1.0 - alpha) / a0;
    }

    // White noise power through the filter is its variance times the
    // impulse response energy.
    clearFilterState();
    double impulseEnergy = 0.0;
    for (int n = 0; n < 4096; ++n) {
        const double response = highPass(n == 0 ? 1.0 : 0.0);
        impulseEnergy += response * response;
    }
    clearFilterState();
    openChannelNoisePower = impulseEnergy / 3.0;
    return true;
}

bool NBFMNoiseDetector::process(const std::vector<float>& discriminator,
                                int sampleRate,
                                bool discontinuity,
                                float& qualityOut) {
    if (discriminator.empty() || !configure(sampleRate)) {
        return false;
    }
    if (discontinuity) {
        initialized = false;
        filteredQuality = 0.0f;
        clearFilterState();
    }

    double highEnergy = 0.0;
    for (float input : discriminator) {
        if (!std::isfinite(input)) {
            reset();
            return false;
        }
        const double high = highPass(input);
        highEnergy += high * high;
    }

    // Absolute noise above the voice band, not its share of the total: a
    // carrier between words has little energy of any kind, yet is clean.
    const double noise = highEnergy / static_cast<double>(discriminator.size()) /
                         openChannelNoisePower;
    const float blockQuality = static_cast<float>(std::clamp(
        (kNBFMNoisyNoise - noise) / (kNBFMNoisyNoise - kNBFMCleanNoise), 0.0, 1.0));

    if (!initialized) {
        filteredQuality = blockQuality;
        initialized = true;
    } else {
        const double blockSeconds = static_cast<double>(discriminator.size()) /
                                    static_cast<double>(sampleRate);
        const double timeConstant = blockQuality < filteredQuality
            ? kNBFMQualityFallSeconds : kNBFMQualityRiseSeconds;
        const double alpha = 1.0 - std::exp(-std::min(blockSeconds, 1.0) / timeConstant);
        filteredQuality += static_cast<float>((blockQuality - filteredQuality) * alpha);
    }
    qualityOut = filteredQuality;
    return true;
}

void SquelchAudioGate::reset() {
    history.clear();
    historyWrite = 0;
    historyCount = 0;
    configuredSampleRate = 0;
    configuredChannels = 0;
    wasOpen = false;
}

void SquelchAudioGate::configure(int sampleRate, int channels) {
    if (sampleRate == configuredSampleRate && channels == configuredChannels) {
        return;
    }
    configuredSampleRate = sampleRate;
    configuredChannels = channels;
    const std::size_t frames = static_cast<std::size_t>(
        std::max(1.0, std::ceil(sampleRate * kPreRollSeconds)));
    history.assign(frames * static_cast<std::size_t>(channels), 0.0f);
    historyWrite = 0;
    historyCount = 0;
    wasOpen = false;
}

void SquelchAudioGate::appendHistory(const std::vector<float>& input) {
    if (history.empty() || input.empty()) {
        return;
    }
    if (input.size() >= history.size()) {
        std::copy(input.end() - static_cast<std::ptrdiff_t>(history.size()), input.end(), history.begin());
        historyWrite = 0;
        historyCount = history.size();
        return;
    }

    const std::size_t first = std::min(input.size(), history.size() - historyWrite);
    std::copy_n(input.begin(), first, history.begin() + static_cast<std::ptrdiff_t>(historyWrite));
    if (first < input.size()) {
        std::copy(input.begin() + static_cast<std::ptrdiff_t>(first), input.end(), history.begin());
    }
    historyWrite = (historyWrite + input.size()) % history.size();
    historyCount = std::min(history.size(), historyCount + input.size());
}

void SquelchAudioGate::copyHistory(std::vector<float>& output) const {
    if (historyCount == 0) {
        return;
    }
    const std::size_t start = historyCount == history.size() ? historyWrite : 0;
    const std::size_t first = std::min(historyCount, history.size() - start);
    output.insert(output.end(),
                  history.begin() + static_cast<std::ptrdiff_t>(start),
                  history.begin() + static_cast<std::ptrdiff_t>(start + first));
    if (first < historyCount) {
        output.insert(output.end(), history.begin(),
                      history.begin() + static_cast<std::ptrdiff_t>(historyCount - first));
    }
}

void SquelchAudioGate::fadeIn(std::vector<float>& samples, int sampleRate, int channels) {
    const std::size_t frames = samples.size() / static_cast<std::size_t>(channels);
    const std::size_t fadeFrames = std::min(frames, static_cast<std::size_t>(
        std::max(1.0, std::ceil(sampleRate * kFadeSeconds))));
    for (std::size_t frame = 0; frame < fadeFrames; ++frame) {
        const float gain = fadeFrames == 1 ? 1.0f
            : static_cast<float>(frame) / static_cast<float>(fadeFrames - 1);
        for (int channel = 0; channel < channels; ++channel) {
            samples[frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)] *= gain;
        }
    }
}

void SquelchAudioGate::fadeOut(std::vector<float>& samples, int sampleRate, int channels) {
    const std::size_t frames = samples.size() / static_cast<std::size_t>(channels);
    const std::size_t fadeFrames = std::min(frames, static_cast<std::size_t>(
        std::max(1.0, std::ceil(sampleRate * kFadeSeconds))));
    const std::size_t firstFrame = frames - fadeFrames;
    for (std::size_t offset = 0; offset < fadeFrames; ++offset) {
        const float gain = fadeFrames == 1 ? 0.0f
            : 1.0f - static_cast<float>(offset) / static_cast<float>(fadeFrames - 1);
        for (int channel = 0; channel < channels; ++channel) {
            samples[(firstFrame + offset) * static_cast<std::size_t>(channels) +
                    static_cast<std::size_t>(channel)] *= gain;
        }
    }
}

SquelchAudioResult SquelchAudioGate::process(const std::vector<float>& input,
                                              int sampleRate,
                                              int channels,
                                              bool enabled,
                                              bool open,
                                              bool discontinuity,
                                              std::vector<float>& output) {
    output.clear();
    if (!enabled || sampleRate <= 0 || channels <= 0 || input.empty()) {
        reset();
        return SquelchAudioResult::PassThrough;
    }
    if (discontinuity) {
        reset();
    }
    configure(sampleRate, channels);

    if (open) {
        if (!wasOpen) {
            output.reserve(historyCount + input.size());
            copyHistory(output);
            output.insert(output.end(), input.begin(), input.end());
            fadeIn(output, sampleRate, channels);
            historyCount = 0;
            historyWrite = 0;
            wasOpen = true;
            return SquelchAudioResult::Processed;
        }
        return SquelchAudioResult::PassThrough;
    }

    if (wasOpen) {
        const std::size_t availableFrames = input.size() / static_cast<std::size_t>(channels);
        const std::size_t fadeFrames = std::min(availableFrames, static_cast<std::size_t>(
            std::max(1.0, std::ceil(sampleRate * kFadeSeconds))));
        const std::size_t fadeSamples = fadeFrames * static_cast<std::size_t>(channels);
        output.assign(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(fadeSamples));
        fadeOut(output, sampleRate, channels);
        wasOpen = false;
        historyCount = 0;
        historyWrite = 0;
        return SquelchAudioResult::Processed;
    }

    appendHistory(input);
    return SquelchAudioResult::Suppress;
}

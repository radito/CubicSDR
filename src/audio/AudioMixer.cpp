// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "AudioThread.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
// Largest playback-rate trim: 500 ppm is under one cent of pitch, yet covers
// any realistic mismatch between SDR and sound card crystals.
constexpr double kMaxRateTrim = 500.0e-6;
// The ring's minimum fill must stray this far from target before trimming
// starts, and come back this close before playback returns to exact rate.
// Excess latency can wait; a shortfall eats the jitter margin (10 ms by
// default) that stands between the ring and an underflow.
constexpr double kTrimStartExcessSeconds = 0.010;
constexpr double kTrimStartShortfallSeconds = 0.004;
constexpr double kTrimStopSeconds = 0.002;
// Seconds over which a fill error is worked off (before the trim limit).
constexpr double kTrimResponseSeconds = 10.0;
// A restart finding more than this beyond the startup cushion is a stall
// backlog and is skipped. Squelch pre-roll (about 67 ms) stays below it.
constexpr double kCatchUpSeconds = 0.100;
// A gap this short that refills by itself was jitter, not a stream ending.
constexpr double kJitterGapSeconds = 0.200;
// Windowed-sinc fractional-delay table, flat to about 19 kHz at 48 kHz so
// trimming never modulates audible treble. Phase 0 is an exact impulse:
// untrimmed playback stays bit-exact.
constexpr size_t kTaps = 16;
constexpr size_t kPhases = 1024;
// Frames held in the window from the current one onward.
constexpr size_t kPrimeFrames = kTaps / 2 + 1;

const std::vector<float>& interpolationTable() {
  static const std::vector<float> table = [] {
    constexpr double pi = 3.14159265358979323846;
    std::vector<float> result(kPhases * kTaps, 0.0f);
    result[kTaps / 2 - 1] = 1.0f;
    for (size_t phase = 1; phase < kPhases; ++phase) {
      const double fraction = static_cast<double>(phase) / kPhases;
      double coefficients[kTaps];
      double sum = 0.0;
      for (size_t tap = 0; tap < kTaps; ++tap) {
        const double t = static_cast<double>(tap) - (kTaps / 2 - 1) - fraction;
        const double sinc = std::sin(pi * t) / (pi * t);
        const double w = 0.42 + 0.5 * std::cos(pi * t / (kTaps / 2)) +
                         0.08 * std::cos(2.0 * pi * t / (kTaps / 2));
        coefficients[tap] = sinc * w;
        sum += coefficients[tap];
      }
      for (size_t tap = 0; tap < kTaps; ++tap) {
        result[phase * kTaps + tap] = static_cast<float>(coefficients[tap] / sum);
      }
    }
    return result;
  }();
  return table;
}
}

AudioStereoRing::AudioStereoRing(size_t capacityFramesIn)
    : samples(capacityFramesIn * 2, 0.0f), capacityFrames(capacityFramesIn) {}

bool AudioStereoRing::push(float left, float right) {
  const size_t write = writePosition.load(std::memory_order_relaxed);
  const size_t read = readPosition.load(std::memory_order_acquire);
  if (write - read >= capacityFrames) return false;
  const size_t index = (write % capacityFrames) * 2;
  samples[index] = left;
  samples[index + 1] = right;
  writePosition.store(write + 1, std::memory_order_release);
  return true;
}

bool AudioStereoRing::pop(float& left, float& right) {
  size_t read = readPosition.load(std::memory_order_relaxed);
  const size_t discard = discardBefore.load(std::memory_order_acquire);
  if (read < discard) {
    read = discard;
    readPosition.store(read, std::memory_order_release);
  }
  const size_t write = writePosition.load(std::memory_order_acquire);
  if (read == write) return false;
  const size_t index = (read % capacityFrames) * 2;
  left = samples[index];
  right = samples[index + 1];
  readPosition.store(read + 1, std::memory_order_release);
  return true;
}

size_t AudioStereoRing::pop(float* interleaved, size_t frames) {
  size_t read = readPosition.load(std::memory_order_relaxed);
  const size_t discard = discardBefore.load(std::memory_order_acquire);
  if (read < discard) read = discard;
  const size_t write = writePosition.load(std::memory_order_acquire);
  const size_t count = std::min(frames, write - read);
  const size_t start = read % capacityFrames;
  const size_t first = std::min(count, capacityFrames - start);
  std::memcpy(interleaved, &samples[start * 2], first * 2 * sizeof(float));
  std::memcpy(interleaved + first * 2, samples.data(), (count - first) * 2 * sizeof(float));
  readPosition.store(read + count, std::memory_order_release);
  return count;
}

size_t AudioStereoRing::discard(size_t frames) {
  size_t read = readPosition.load(std::memory_order_relaxed);
  const size_t flushed = discardBefore.load(std::memory_order_acquire);
  if (read < flushed) read = flushed;
  const size_t count = std::min(frames, writePosition.load(std::memory_order_acquire) - read);
  readPosition.store(read + count, std::memory_order_release);
  return count;
}

size_t AudioStereoRing::available() {
  size_t read = readPosition.load(std::memory_order_relaxed);
  const size_t discard = discardBefore.load(std::memory_order_acquire);
  if (read < discard) {
    read = discard;
    readPosition.store(read, std::memory_order_release);
  }
  return writePosition.load(std::memory_order_acquire) - read;
}

size_t AudioStereoRing::producerAvailable() const {
  const size_t read = readPosition.load(std::memory_order_acquire);
  const size_t discard = discardBefore.load(std::memory_order_acquire);
  return writePosition.load(std::memory_order_acquire) - std::max(read, discard);
}

size_t AudioStereoRing::capacity() const { return capacityFrames; }

void AudioStereoRing::clear() {
  // Only the consumer writes readPosition. Publishing a boundary avoids a
  // data race if CoreAudio is already reading the frame being discarded.
  discardBefore.store(writePosition.load(std::memory_order_acquire), std::memory_order_release);
}

AudioMixState::AudioMixState()
    : scratch((static_cast<size_t>(kRenderChunkFrames * (1.0 + kMaxRateTrim)) + 2) * 2) {
  // Build the shared table here, never inside the real-time callback.
  interpolationTable();
}

void AudioMixState::startPlayout() {
  // Playback starts from silence: the past half of the window is zero and
  // the first output is exactly the first buffered frame.
  history.fill(0.0f);
  historyHead = 0;
  float first[kPrimeFrames * 2];
  ring.pop(first, kPrimeFrames);
  for (size_t i = 0; i < kPrimeFrames; ++i) {
    for (size_t copy : {kTaps / 2 - 1 + i, kTaps + kTaps / 2 - 1 + i}) {
      history[copy * 2] = first[i * 2];
      history[copy * 2 + 1] = first[i * 2 + 1];
    }
  }
  position = 0.0;
  readRatio = 1.0;
  correcting = false;
  windowMinimumFill = std::numeric_limits<size_t>::max();
  windowFillSum = 0;
  windowCallbacks = 0;
  windowFrames = 0;
  rateTrimPpm.store(0.0f, std::memory_order_relaxed);
}

void AudioMixState::updateRateTrim(size_t callbackFrames, int rate) {
  const double target = static_cast<double>(callbackFrames + jitterMarginFrames);
  const double excess = static_cast<double>(windowMinimumFill) - target;
  if (excess > kTrimStartExcessSeconds * rate || -excess > kTrimStartShortfallSeconds * rate) {
    correcting = true;
  } else if (std::fabs(excess) < kTrimStopSeconds * rate) {
    correcting = false;
  }
  readRatio = correcting
      ? 1.0 + std::clamp(excess / (kTrimResponseSeconds * rate), -kMaxRateTrim, kMaxRateTrim)
      : 1.0;

  // A jitter-driven margin increase relaxes slowly once things are calm.
  const size_t defaultMargin = static_cast<size_t>(rate / 100);
  const size_t decay = std::max<size_t>(1, static_cast<size_t>(rate / 10000));
  jitterMarginFrames = std::max(defaultMargin,
                                jitterMarginFrames > decay ? jitterMarginFrames - decay : 0);

  bufferedMs.store(static_cast<float>(1000.0 * windowFillSum / windowCallbacks / rate),
                   std::memory_order_relaxed);
  rateTrimPpm.store(static_cast<float>((readRatio - 1.0) * 1.0e6), std::memory_order_relaxed);
  windowMinimumFill = std::numeric_limits<size_t>::max();
  windowFillSum = 0;
  windowCallbacks = 0;
  windowFrames = 0;
}

void AudioMixState::render(float* out, size_t frames) {
  const int rate = sampleRate.load(std::memory_order_relaxed);
  if (rate <= 0 || frames == 0) return;
  if (jitterMarginFrames == 0) jitterMarginFrames = static_cast<size_t>(rate / 100);
  if (starving) starvedFrames += frames;

  size_t fill = ring.available();
  if (!primed.load(std::memory_order_acquire)) {
    const size_t target = frames + jitterMarginFrames;
    const size_t startup = std::max({frames * 2, static_cast<size_t>(rate / 50), target}) +
                           kPrimeFrames;
    if (fill < startup) return;
    const size_t catchUp = static_cast<size_t>(kCatchUpSeconds * rate);
    if (fill > startup + catchUp) {
      ring.discard(fill - startup);
      catchUps.fetch_add(1, std::memory_order_relaxed);
      fill = startup;
    }
    if (starving && starvedFrames < kJitterGapSeconds * rate) {
      dropouts.fetch_add(1, std::memory_order_relaxed);
      jitterMarginFrames = std::min(jitterMarginFrames + static_cast<size_t>(rate / 200),
                                    static_cast<size_t>(rate / 10));
    }
    starving = false;
    startPlayout();
    fill -= kPrimeFrames;
    primed.store(true, std::memory_order_release);
  }

  windowMinimumFill = std::min(windowMinimumFill, fill);
  windowFillSum += fill;
  ++windowCallbacks;
  windowFrames += frames;
  if (windowFrames >= static_cast<size_t>(rate)) updateRateTrim(frames, rate);

  const float sourceGain = gain.load(std::memory_order_relaxed);
  const float* table = interpolationTable().data();
  for (size_t done = 0; done < frames;) {
    const size_t chunk = std::min(frames - done, kRenderChunkFrames);
    // Replay the position arithmetic to learn exactly how many input frames
    // this chunk consumes, then take them from the ring in one read.
    double probe = position;
    size_t needed = 0;
    for (size_t i = 0; i < chunk; ++i) {
      probe += readRatio;
      while (probe >= 1.0) {
        probe -= 1.0;
        ++needed;
      }
    }
    const size_t got = ring.pop(scratch.data(), needed);

    size_t used = 0;
    float* frame = out + done * 2;
    for (size_t i = 0; i < chunk; ++i, frame += 2) {
      const float* taps = table + static_cast<size_t>(position * kPhases) * kTaps;
      const float* past = history.data() + historyHead * 2;
      float left = 0.0f;
      float right = 0.0f;
      for (size_t tap = 0; tap < kTaps; ++tap) {
        left += taps[tap] * past[tap * 2];
        right += taps[tap] * past[tap * 2 + 1];
      }
      frame[0] += sourceGain * left;
      frame[1] += sourceGain * right;
      position += readRatio;
      while (position >= 1.0) {
        if (used == got) {
          underflows.fetch_add(1, std::memory_order_relaxed);
          primed.store(false, std::memory_order_release);
          starving = true;
          starvedFrames = 0;
          return;
        }
        for (size_t copy : {historyHead, historyHead + kTaps}) {
          history[copy * 2] = scratch[used * 2];
          history[copy * 2 + 1] = scratch[used * 2 + 1];
        }
        historyHead = (historyHead + 1) % kTaps;
        ++used;
        position -= 1.0;
      }
    }
    done += chunk;
  }
}

void AudioMixState::clearUnlocked() {
  ring.clear();
  primed.store(false, std::memory_order_release);
  havePrevious = false;
  phase = 0.0;
  resamplerInputRate = 0;
  resamplerOutputRate = 0;
}

void AudioMixState::clear() {
  std::lock_guard<std::mutex> lock(producerMutex);
  clearUnlocked();
}

void AudioMixState::queue(const AudioThreadInput& input) {
  std::lock_guard<std::mutex> lock(producerMutex);
  if (input.discontinuity) {
    // Upstream DSP state has already been reset at the IQ gap. Do not discard
    // audio that is already queued in chronological order: remote SDR drivers
    // may report consecutive discontinuities, and flushing here on every
    // packet prevents the ring from ever reaching its playback threshold.
    havePrevious = false;
    phase = 0.0;
    resamplerInputRate = 0;
    resamplerOutputRate = 0;
  }
  if (input.channels != 1 && input.channels != 2) return;
  const int outputSampleRate = sampleRate.load(std::memory_order_acquire);
  if (input.sampleRate <= 0 || outputSampleRate <= 0) return;

  const size_t frames = input.data.size() / static_cast<size_t>(input.channels);
  if (frames == 0) return;

  auto pushFrame = [this](float left, float right) {
    if (ring.push(left, right)) return true;
    // Leave an already-primed, full ring playable. Marking it unprimed here
    // made an ordinary producer burst turn into an audible dropout.
    overflows.fetch_add(1, std::memory_order_relaxed);
    return false;
  };

  // Demodulators are normally built for the CoreAudio device rate. Preserve
  // those samples exactly: continuously varying a linear-resampler ratio here
  // frequency-modulated speech and produced the characteristic robotic sound.
  if (input.sampleRate == outputSampleRate) {
    havePrevious = false;
    phase = 0.0;
    resamplerInputRate = 0;
    resamplerOutputRate = 0;
    for (size_t frame = 0; frame < frames; ++frame) {
      const float left = input.data[frame * input.channels];
      const float right = input.channels == 2 ? input.data[frame * 2 + 1] : left;
      if (!pushFrame(left, right)) return;
    }
    return;
  }

  // Rate mismatches only occur briefly while an output-device change rebuilds
  // the demodulator kit. Convert at the fixed nominal ratio so audio continues
  // without pitch modulation during that transition.
  if (resamplerInputRate != input.sampleRate || resamplerOutputRate != outputSampleRate) {
    havePrevious = false;
    phase = 0.0;
    resamplerInputRate = input.sampleRate;
    resamplerOutputRate = outputSampleRate;
  }
  const double step = static_cast<double>(input.sampleRate) /
      static_cast<double>(outputSampleRate);

  for (size_t frame = 0; frame < frames; ++frame) {
    const float left = input.data[frame * input.channels];
    const float right = input.channels == 2 ? input.data[frame * 2 + 1] : left;
    if (!havePrevious) {
      previousLeft = left;
      previousRight = right;
      havePrevious = true;
      continue;
    }

    while (phase < 1.0) {
      const float outLeft = previousLeft + (left - previousLeft) * static_cast<float>(phase);
      const float outRight = previousRight + (right - previousRight) * static_cast<float>(phase);
      if (!pushFrame(outLeft, outRight)) return;
      phase += step;
    }
    phase -= 1.0;
    previousLeft = left;
    previousRight = right;
  }
}

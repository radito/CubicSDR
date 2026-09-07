// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "AudioThread.h"

#include <algorithm>

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

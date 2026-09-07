// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

// Collects arbitrarily sized sample chunks into a fixed-size frame. Once the
// first frame is complete, each append produces a rolling frame containing the
// newest samples.
template <typename Sample>
class SampleFrameAccumulator {
public:
    explicit SampleFrameAccumulator(std::size_t frameSize = 0) {
        reset(frameSize);
    }

    void reset(std::size_t frameSize) {
        frameSize_ = frameSize;
        buffered_ = 0;
        history_.assign(frameSize_, Sample{});
    }

    std::size_t frameSize() const {
        return frameSize_;
    }

    std::size_t bufferedSamples() const {
        return buffered_;
    }

    bool append(const Sample *samples, std::size_t count, Sample *frame) {
        if (frameSize_ == 0 || frame == nullptr || samples == nullptr || count == 0) {
            return false;
        }

        if (count >= frameSize_) {
            const Sample *newestFrame = samples + (count - frameSize_);
            std::copy(newestFrame, newestFrame + frameSize_, frame);
        } else if (buffered_ + count < frameSize_) {
            std::copy(samples, samples + count, history_.begin() + buffered_);
            buffered_ += count;
            return false;
        } else {
            const std::size_t oldSampleCount = frameSize_ - count;
            const std::size_t oldSampleStart = buffered_ - oldSampleCount;
            std::copy(history_.begin() + oldSampleStart, history_.begin() + buffered_, frame);
            std::copy(samples, samples + count, frame + oldSampleCount);
        }

        std::copy(frame, frame + frameSize_, history_.begin());
        buffered_ = frameSize_;
        return true;
    }

private:
    std::size_t frameSize_ = 0;
    std::size_t buffered_ = 0;
    std::vector<Sample> history_;
};

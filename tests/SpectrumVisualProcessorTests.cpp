// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "SpectrumVisualProcessor.h"

#include <algorithm>
#include <cmath>

namespace {
struct SpectrumFixture {
    SpectrumVisualProcessor processor;
    DemodulatorThreadInputQueuePtr input = std::make_shared<DemodulatorThreadInputQueue>();
    SpectrumVisualDataQueuePtr output = std::make_shared<SpectrumVisualDataQueue>();

    SpectrumFixture() {
        processor.setup(256);
        processor.setInput(input);
        processor.attachOutput(output);
    }

    SpectrumVisualDataPtr feed(size_t count, size_t offset, bool gap,
                               long long rate = 12000, float tone = 1000.0f) {
        auto iq = std::make_shared<DemodulatorThreadIQData>();
        iq->sampleRate = rate;
        iq->frequency = 1000000;
        iq->discontinuity = gap;
        iq->data.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const double phase = 2.0 * M_PI * tone * (offset + i) / rate;
            iq->data[i].real = std::cos(phase);
            iq->data[i].imag = std::sin(phase);
        }
        CUBIC_REQUIRE(input->try_push(iq));
        processor.run();
        SpectrumVisualDataPtr result;
        output->try_pop(result);
        return result;
    }
};

void requireFiniteSpectrum(const SpectrumVisualDataPtr& frame) {
    CUBIC_REQUIRE(frame != nullptr);
    CUBIC_REQUIRE(frame->spectrum_points.size() == 512);
    CUBIC_REQUIRE(std::isfinite(frame->fft_floor));
    CUBIC_REQUIRE(std::isfinite(frame->fft_ceiling));
    for (float value : frame->spectrum_points) CUBIC_REQUIRE(std::isfinite(value));
}
}

CUBIC_TEST(demod_thumbnail_accumulates_small_blocks_and_recovers_after_a_gap) {
    SpectrumFixture fixture;
    fixture.processor.setView(true, 1000000, 6000);
    size_t frames = 0;
    // Narrow AM supplies fewer samples per packet than the thumbnail FFT.
    // Both before and after a real gap, subsequent packets must make progress.
    for (size_t block = 0; block < 80; ++block) {
        auto frame = fixture.feed(64, block * 64, block == 0 || block == 40);
        if (frame) {
            requireFiniteSpectrum(frame);
            ++frames;
        }
        if (block == 39) {
            CUBIC_REQUIRE(frames > 10);
            frames = 0;
        }
    }
    CUBIC_REQUIRE(frames > 10);
}

CUBIC_TEST(zoomed_demod_thumbnail_keeps_its_tuned_frequency_after_a_gap) {
    SpectrumFixture continuous;
    SpectrumFixture reset;
    continuous.processor.setView(true, 1006000, 12000);
    reset.processor.setView(true, 1006000, 12000);
    SpectrumVisualDataPtr expected, actual;
    for (size_t block = 0; block < 40; ++block) {
        // Integer tone cycles per block make a phase reset harmless; losing
        // the 6 kHz oscillator frequency moves the peak and fails comparison.
        expected = continuous.feed(2048, block * 2048, false, 48000, 6000);
        actual = reset.feed(2048, block * 2048, block == 8, 48000, 6000);
    }
    requireFiniteSpectrum(expected);
    requireFiniteSpectrum(actual);
    auto peakBin = [](const SpectrumVisualDataPtr& frame) {
        size_t peak = 0;
        for (size_t bin = 1; bin < frame->spectrum_points.size() / 2; ++bin) {
            if (frame->spectrum_points[bin * 2 + 1] > frame->spectrum_points[peak * 2 + 1]) peak = bin;
        }
        return peak;
    };
    CUBIC_REQUIRE(peakBin(expected) == peakBin(actual));
}

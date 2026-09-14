// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "RNNoiseLegacy.h"

#include <array>
#include <cmath>
#include <cstdio>

extern "C" {
#include "rnnoise.h"
}

CUBIC_TEST(rnnoise_bundled_model_loads_and_processes_audio) {
    FILE *modelFile = std::fopen(CUBICSDR_RNNOISE_TEST_MODEL, "rb");
    CUBIC_REQUIRE(modelFile != nullptr);

    RNNModel *model = rnnoise_model_from_file(modelFile);
    CUBIC_REQUIRE(model != nullptr);
    DenoiseState *state = rnnoise_create(model);
    CUBIC_REQUIRE(state != nullptr);
    CUBIC_REQUIRE(rnnoise_get_frame_size() == 480);

    std::array<float, 480> input{};
    std::array<float, 480> output{};
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = 1000.0f * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / 48000.0);
    }

    const float vad = rnnoise_process_frame(state, output.data(), input.data());
    CUBIC_REQUIRE(std::isfinite(vad));
    for (float sample : output) {
        CUBIC_REQUIRE(std::isfinite(sample));
    }

    rnnoise_destroy(state);
    rnnoise_model_free(model);
    std::fclose(modelFile);
}

CUBIC_TEST(rnnoise_legacy_model_loads_and_processes_audio) {
    RNNoiseLegacyState *state = rnnoise_legacy_create(nullptr);
    CUBIC_REQUIRE(state != nullptr);
    CUBIC_REQUIRE(rnnoise_legacy_get_frame_size() == 480);

    std::array<float, 480> frame{};
    for (size_t i = 0; i < frame.size(); ++i) {
        frame[i] = 1000.0f * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / 48000.0);
    }

    const float vad = rnnoise_legacy_process_frame(state, frame.data(), frame.data());
    CUBIC_REQUIRE(std::isfinite(vad));
    for (float sample : frame) {
        CUBIC_REQUIRE(std::isfinite(sample));
    }

    rnnoise_legacy_destroy(state);
}

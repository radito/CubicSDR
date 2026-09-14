// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#pragma once

extern "C" {

typedef struct RNNoiseLegacyState RNNoiseLegacyState;

int rnnoise_legacy_get_frame_size(void);
RNNoiseLegacyState* rnnoise_legacy_create(void* model);
void rnnoise_legacy_destroy(RNNoiseLegacyState* state);
float rnnoise_legacy_process_frame(RNNoiseLegacyState* state,
                                   float* output,
                                   const float* input);

}

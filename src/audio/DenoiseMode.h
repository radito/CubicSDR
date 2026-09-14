// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#pragma once

enum class DenoiseMode : int {
  Off = 0,
  Mid = 1,
  Strong = 2,
};

inline bool isDenoiseEnabled(DenoiseMode mode) {
  return mode != DenoiseMode::Off;
}

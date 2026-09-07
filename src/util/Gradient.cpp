// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "Gradient.h"
#include <algorithm>
#include <cstddef>

Gradient::Gradient() = default;

void Gradient::clear() {
	colors.clear();
}

void Gradient::addColor(GradientColor c) {
    colors.push_back(c);
}

void Gradient::addColors(const std::vector<GradientColor>& color_list) {

	for (auto single_color : color_list) {

		colors.push_back(single_color);
	}
}

std::vector<float> &Gradient::getRed() {
    return r_val;
}

std::vector<float> &Gradient::getGreen() {
    return g_val;
}

std::vector<float> &Gradient::getBlue() {
    return b_val;
}

void Gradient::generate(unsigned int len) {
    r_val.resize(len);
    g_val.resize(len);
    b_val.resize(len);

    if (len == 0) {
        return;
    }

    if (colors.empty()) {
        std::fill(r_val.begin(), r_val.end(), 0.0f);
        std::fill(g_val.begin(), g_val.end(), 0.0f);
        std::fill(b_val.begin(), b_val.end(), 0.0f);
        return;
    }

    if (colors.size() == 1 || len == 1) {
        const float r = std::clamp(colors.front().r, 0.0f, 1.0f);
        const float g = std::clamp(colors.front().g, 0.0f, 1.0f);
        const float b = std::clamp(colors.front().b, 0.0f, 1.0f);
        std::fill(r_val.begin(), r_val.end(), r);
        std::fill(g_val.begin(), g_val.end(), g);
        std::fill(b_val.begin(), b_val.end(), b);
        return;
    }

    const float segmentCount = static_cast<float>(colors.size() - 1);
    for (size_t i = 0; i < len; ++i) {
        const float position = static_cast<float>(i) * segmentCount / static_cast<float>(len - 1);
        const size_t segment = std::min(static_cast<size_t>(position), colors.size() - 2);
        const float amount = position - static_cast<float>(segment);

        r_val[i] = std::clamp(colors[segment].r + (colors[segment + 1].r - colors[segment].r) * amount, 0.0f, 1.0f);
        g_val[i] = std::clamp(colors[segment].g + (colors[segment + 1].g - colors[segment].g) * amount, 0.0f, 1.0f);
        b_val[i] = std::clamp(colors[segment].b + (colors[segment + 1].b - colors[segment].b) * amount, 0.0f, 1.0f);
    }
}

Gradient::~Gradient() = default;

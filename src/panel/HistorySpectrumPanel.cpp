// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "HistorySpectrumPanel.h"

#include <algorithm>
#include <cmath>
#include <utility>

HistorySpectrumPanel::HistorySpectrumPanel() {
    setFill(GLPANEL_FILL_GRAD_Y);
    setFillColor(ThemeMgr::mgr.currentTheme->fftBackground * 2.0f,
                 ThemeMgr::mgr.currentTheme->fftBackground);
}

void HistorySpectrumPanel::clear() {
    history.clear();
    lastAppend = {};
}

bool HistorySpectrumPanel::beginAppend(float floorDb, float ceilDb, int streamRate) {
    if (!std::isfinite(floorDb) || !std::isfinite(ceilDb)) return false;
    const auto now = std::chrono::steady_clock::now();
    if (lastAppend.time_since_epoch().count() &&
        now - lastAppend < std::chrono::milliseconds(HISTORY_INTERVAL_MS)) return false;
    if (lastSampleRate && lastSampleRate != streamRate) clear();
    lastSampleRate = streamRate;
    lastAppend = now;
    floorValue = floorDb;
    ceilValue = ceilDb;
    return true;
}

void HistorySpectrumPanel::appendRow(std::vector<float>&& row) {
    history.push_back(std::move(row));
    if (history.size() > HISTORY_ROWS) history.pop_front();
}

void HistorySpectrumPanel::addSpectrum(const std::vector<float>& db,
                                       float floorDb, float ceilDb,
                                       int sampleRate) {
    if (db.empty() || !beginAppend(floorDb, ceilDb, sampleRate)) return;

    std::vector<float> row(HISTORY_BINS, floorDb);
    for (size_t bin = 0; bin < HISTORY_BINS; ++bin) {
        const size_t first = bin * db.size() / HISTORY_BINS;
        const size_t last = std::max(first + 1, (bin + 1) * db.size() / HISTORY_BINS);
        for (size_t source = first; source < std::min(last, db.size()); ++source) {
            row[bin] = std::max(row[bin], db[source]);
        }
    }
    appendRow(std::move(row));
}

void HistorySpectrumPanel::addSpectrumPoints(const std::vector<float>& points,
                                             float floorDb, float ceilDb,
                                             int streamRate) {
    const size_t sourceBins = points.size() / 2;
    if (!sourceBins || !beginAppend(floorDb, ceilDb, streamRate)) return;

    const float range = std::max(ceilDb - floorDb, 10.0f);
    std::vector<float> row(HISTORY_BINS, floorDb);
    for (size_t bin = 0; bin < HISTORY_BINS; ++bin) {
        const size_t first = bin * sourceBins / HISTORY_BINS;
        const size_t last = std::max(first + 1, (bin + 1) * sourceBins / HISTORY_BINS);
        float peak = 0.0f;
        for (size_t source = first; source < std::min(last, sourceBins); ++source) {
            peak = std::max(peak, points[source * 2 + 1]);
        }
        row[bin] = floorDb + std::clamp(peak, 0.0f, 1.0f) * range;
    }
    appendRow(std::move(row));
}

void HistorySpectrumPanel::drawPanelContents() {
    if (history.size() < 2) return;
    const float range = std::max(ceilValue - floorValue, 10.0f);
    auto& gradient = ThemeMgr::mgr.currentTheme->waterfallGradient;
    const auto& red = gradient.getRed();
    const auto& green = gradient.getGreen();
    const auto& blue = gradient.getBlue();
    if (red.empty()) return;

    glPushMatrix();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);

    const auto rowAge = [&](size_t row) {
        return static_cast<float>(history.size() - 1 - row) /
               static_cast<float>(HISTORY_ROWS - 1);
    };
    const auto projectedPoint = [&](size_t row, size_t bin, float value) {
        const float age = rowAge(row);
        const float halfWidth = 0.94f - age * 0.22f;
        const float centerX = historyShiftX * age;
        const float x = centerX - halfWidth + 2.0f * halfWidth * static_cast<float>(bin) /
                                                   static_cast<float>(HISTORY_BINS - 1);
        const float baseline = 0.62f - age * 0.88f;
        const float y = baseline - value * 0.52f;
        return std::pair<float, float>(x, yAxisUp ? -y : y);
    };

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.55f, 0.55f, 0.55f, 0.34f);
    glBegin(GL_LINES);
    for (int line = 0; line <= 4; ++line) {
        const size_t bin = line * (HISTORY_BINS - 1) / 4;
        const auto front = projectedPoint(history.size() - 1, bin, 0.0f);
        const auto back = projectedPoint(0, bin, 0.0f);
        glVertex2f(front.first, front.second);
        glVertex2f(back.first, back.second);
    }
    for (size_t row = 0; row < history.size(); row += 12) {
        const auto left = projectedPoint(row, 0, 0.0f);
        const auto right = projectedPoint(row, HISTORY_BINS - 1, 0.0f);
        glVertex2f(left.first, left.second);
        glVertex2f(right.first, right.second);
    }
    glEnd();
    glDisable(GL_BLEND);

    auto vertex = [&](size_t row, size_t bin) {
        const float value = std::clamp((history[row][bin] - floorValue) / range, 0.0f, 1.0f);
        const size_t color = std::min(red.size() - 1,
            static_cast<size_t>(value * static_cast<float>(red.size() - 1)));
        const float light = 0.60f + (1.0f - rowAge(row)) * 0.40f;
        glColor3f(red[color] * light, green[color] * light, blue[color] * light);
        const auto point = projectedPoint(row, bin, value);
        glVertex2f(point.first, point.second);
    };

    for (size_t row = 1; row < history.size(); ++row) {
        glBegin(GL_TRIANGLE_STRIP);
        for (size_t bin = 0; bin < HISTORY_BINS; ++bin) {
            vertex(row - 1, bin);
            vertex(row, bin);
        }
        glEnd();
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.03f, 0.03f, 0.03f, 0.28f);
    glBegin(GL_LINES);
    for (size_t row = 0; row < history.size(); row += 4) {
        for (size_t bin = 1; bin < HISTORY_BINS; ++bin) {
            const float value0 = std::clamp((history[row][bin - 1] - floorValue) / range, 0.0f, 1.0f);
            const float value1 = std::clamp((history[row][bin] - floorValue) / range, 0.0f, 1.0f);
            const auto p0 = projectedPoint(row, bin - 1, value0);
            const auto p1 = projectedPoint(row, bin, value1);
            glVertex2f(p0.first, p0.second);
            glVertex2f(p1.first, p1.second);
        }
    }
    glEnd();
    glDisable(GL_BLEND);

    glPopMatrix();
}

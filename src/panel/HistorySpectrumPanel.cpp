// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "HistorySpectrumPanel.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <utility>

HistorySpectrumPanel::HistorySpectrumPanel() {
    setFill(GLPANEL_FILL_GRAD_Y);
    setFillColor(ThemeMgr::mgr.currentTheme->fftBackground * 2.0f,
                 ThemeMgr::mgr.currentTheme->fftBackground);
}

void HistorySpectrumPanel::setLinesPerSecond(int linesPerSecond) {
    const int safeRate = linesPerSecond > 0 ? linesPerSecond : 1;
    appendInterval = std::chrono::microseconds(1000000 / safeRate);
}

void HistorySpectrumPanel::clear() {
    history.clear();
    lastAppend = {};
    geometryDirty = true;
}

bool HistorySpectrumPanel::beginAppend(float floorDb, float ceilDb, int streamRate) {
    if (!std::isfinite(floorDb) || !std::isfinite(ceilDb)) return false;
    const auto now = std::chrono::steady_clock::now();
    if (lastAppend.time_since_epoch().count() &&
        now - lastAppend < appendInterval) return false;
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
    geometryDirty = true;
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

void HistorySpectrumPanel::rebuildGeometry() {
    surfaceVertices.clear();
    lineVertices.clear();
    gridVertexCount = 0;
    if (history.size() < 2) return;

    const float range = std::max(ceilValue - floorValue, 10.0f);
    auto& gradient = ThemeMgr::mgr.currentTheme->waterfallGradient;
    const auto& red = gradient.getRed();
    const auto& green = gradient.getGreen();
    const auto& blue = gradient.getBlue();
    if (red.empty()) return;

    const auto rowAge = [&](size_t row) {
        return static_cast<float>(history.size() - 1 - row) /
               static_cast<float>(HISTORY_ROWS - 1);
    };
    const auto projectedPoint = [&](size_t row, size_t bin, float value) {
        const float age = rowAge(row);
        const float halfWidth = 0.94f - age * 0.22f;
        const float centerX = historyShiftX * age;
        const float x = centerX - halfWidth +
                        2.0f * halfWidth * static_cast<float>(bin) /
                        static_cast<float>(HISTORY_BINS - 1);
        const float baseline = 0.62f - age * 0.88f;
        const float y = baseline - value * 0.52f;
        return std::pair<float, float>(x, yAxisUp ? -y : y);
    };
    const auto addLine = [&](const std::pair<float, float>& a,
                             const std::pair<float, float>& b,
                             float shade, float alpha) {
        lineVertices.push_back({a.first, a.second, shade, shade, shade, alpha});
        lineVertices.push_back({b.first, b.second, shade, shade, shade, alpha});
    };
    const auto makeVertex = [&](size_t row, size_t bin) {
        const float value = std::clamp((history[row][bin] - floorValue) / range,
                                       0.0f, 1.0f);
        const size_t color = std::min(red.size() - 1,
            static_cast<size_t>(value * static_cast<float>(red.size() - 1)));
        const float light = 0.60f + (1.0f - rowAge(row)) * 0.40f;
        const auto point = projectedPoint(row, bin, value);
        return RenderVertex{point.first, point.second,
                            red[color] * light, green[color] * light,
                            blue[color] * light, 1.0f};
    };

    for (int line = 0; line <= 4; ++line) {
        const size_t bin = line * (HISTORY_BINS - 1) / 4;
        addLine(projectedPoint(history.size() - 1, bin, 0.0f),
                projectedPoint(0, bin, 0.0f), 0.55f, 0.34f);
    }
    for (size_t row = 0; row < history.size(); row += 12) {
        addLine(projectedPoint(row, 0, 0.0f),
                projectedPoint(row, HISTORY_BINS - 1, 0.0f), 0.55f, 0.34f);
    }
    gridVertexCount = lineVertices.size();

    surfaceVertices.reserve((history.size() - 1) * (HISTORY_BINS * 2 + 2));
    for (size_t row = 1; row < history.size(); ++row) {
        if (row > 1) {
            surfaceVertices.push_back(surfaceVertices.back());
            surfaceVertices.push_back(makeVertex(row - 1, 0));
        }
        for (size_t bin = 0; bin < HISTORY_BINS; ++bin) {
            surfaceVertices.push_back(makeVertex(row - 1, bin));
            surfaceVertices.push_back(makeVertex(row, bin));
        }
    }

    for (size_t row = 0; row < history.size(); row += 4) {
        for (size_t bin = 1; bin < HISTORY_BINS; ++bin) {
            const float value0 = std::clamp(
                (history[row][bin - 1] - floorValue) / range, 0.0f, 1.0f);
            const float value1 = std::clamp(
                (history[row][bin] - floorValue) / range, 0.0f, 1.0f);
            addLine(projectedPoint(row, bin - 1, value0),
                    projectedPoint(row, bin, value1), 0.03f, 0.28f);
        }
    }
}

void HistorySpectrumPanel::releaseGL() {
#ifdef __APPLE__
    if (surfaceVbo) glDeleteBuffers(1, &surfaceVbo);
    if (lineVbo) glDeleteBuffers(1, &lineVbo);
    surfaceVbo = 0;
    lineVbo = 0;
#endif
}

void HistorySpectrumPanel::drawPanelContents() {
    if (geometryDirty) {
        rebuildGeometry();
#ifdef __APPLE__
        if (!surfaceVbo) glGenBuffers(1, &surfaceVbo);
        if (!lineVbo) glGenBuffers(1, &lineVbo);
        glBindBuffer(GL_ARRAY_BUFFER, surfaceVbo);
        glBufferData(GL_ARRAY_BUFFER,
                     surfaceVertices.size() * sizeof(RenderVertex),
                     surfaceVertices.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, lineVbo);
        glBufferData(GL_ARRAY_BUFFER,
                     lineVertices.size() * sizeof(RenderVertex),
                     lineVertices.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
#endif
        geometryDirty = false;
    }
    if (surfaceVertices.empty()) return;

    glPushMatrix();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

#ifdef __APPLE__
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo);
    glVertexPointer(2, GL_FLOAT, sizeof(RenderVertex),
                    reinterpret_cast<const GLvoid *>(offsetof(RenderVertex, x)));
    glColorPointer(4, GL_FLOAT, sizeof(RenderVertex),
                   reinterpret_cast<const GLvoid *>(offsetof(RenderVertex, r)));
#else
    glVertexPointer(2, GL_FLOAT, sizeof(RenderVertex), &lineVertices.front().x);
    glColorPointer(4, GL_FLOAT, sizeof(RenderVertex), &lineVertices.front().r);
#endif
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(gridVertexCount));

#ifdef __APPLE__
    glBindBuffer(GL_ARRAY_BUFFER, surfaceVbo);
    glVertexPointer(2, GL_FLOAT, sizeof(RenderVertex),
                    reinterpret_cast<const GLvoid *>(offsetof(RenderVertex, x)));
    glColorPointer(4, GL_FLOAT, sizeof(RenderVertex),
                   reinterpret_cast<const GLvoid *>(offsetof(RenderVertex, r)));
#else
    glVertexPointer(2, GL_FLOAT, sizeof(RenderVertex), &surfaceVertices.front().x);
    glColorPointer(4, GL_FLOAT, sizeof(RenderVertex), &surfaceVertices.front().r);
#endif
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLE_STRIP, 0,
                 static_cast<GLsizei>(surfaceVertices.size()));

#ifdef __APPLE__
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo);
    glVertexPointer(2, GL_FLOAT, sizeof(RenderVertex),
                    reinterpret_cast<const GLvoid *>(offsetof(RenderVertex, x)));
    glColorPointer(4, GL_FLOAT, sizeof(RenderVertex),
                   reinterpret_cast<const GLvoid *>(offsetof(RenderVertex, r)));
#else
    glVertexPointer(2, GL_FLOAT, sizeof(RenderVertex), &lineVertices.front().x);
    glColorPointer(4, GL_FLOAT, sizeof(RenderVertex), &lineVertices.front().r);
#endif
    glEnable(GL_BLEND);
    glDrawArrays(GL_LINES, static_cast<GLint>(gridVertexCount),
                 static_cast<GLsizei>(lineVertices.size() - gridVertexCount));

    glDisable(GL_BLEND);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
#ifdef __APPLE__
    glBindBuffer(GL_ARRAY_BUFFER, 0);
#endif
    glPopMatrix();
}

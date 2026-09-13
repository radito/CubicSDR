// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#pragma once

#include "GLPanel.h"

#include <array>
#include <chrono>

class HistorySpectrumPanel : public GLPanel {
public:
    HistorySpectrumPanel();
    void addSpectrum(const std::vector<float>& db, float floorDb, float ceilDb,
                     int sampleRate);
    void addSpectrumPoints(const std::vector<float>& points, float floorDb,
                           float ceilDb, int streamRate);
    void setLinesPerSecond(int linesPerSecond);
    void setYAxisUp(bool enabled) { yAxisUp = enabled; geometryDirty = true; }
    void setHistoryShift(float shiftX) { historyShiftX = shiftX; geometryDirty = true; }
    void clear();
    void releaseGL();
    size_t historySize() const { return historyCount; }

protected:
    void drawPanelContents() override;

private:
    static constexpr size_t HISTORY_BINS = 256;
    static constexpr size_t HISTORY_ROWS = 96;

    using HistoryRow = std::array<float, HISTORY_BINS>;
    std::array<HistoryRow, HISTORY_ROWS> history{};
    size_t historyCount = 0;
    size_t historyHead = 0;
    float floorValue = -100.0f;
    float ceilValue = 0.0f;
    int lastSampleRate = 0;
    bool yAxisUp = false;
    float historyShiftX = 0.0f;
    std::chrono::steady_clock::time_point lastAppend{};
    std::chrono::microseconds appendInterval{40000};

    struct RenderVertex {
        GLfloat x, y;
        GLfloat r, g, b, a;
    };
    std::vector<RenderVertex> surfaceVertices;
    std::vector<RenderVertex> lineVertices;
    size_t gridVertexCount = 0;
    bool geometryDirty = true;
#ifdef __APPLE__
    GLuint surfaceVbo = 0;
    GLuint lineVbo = 0;
    size_t surfaceVboCapacity = 0;
    size_t lineVboCapacity = 0;
#endif

    bool beginAppend(float floorDb, float ceilDb, int streamRate);
    void appendRow(HistoryRow&& row);
    const HistoryRow& historyRow(size_t logicalIndex) const;
    void rebuildGeometry();
};

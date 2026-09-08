// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#pragma once

#include "GLPanel.h"

#include <chrono>
#include <deque>

class HistorySpectrumPanel : public GLPanel {
public:
    HistorySpectrumPanel();
    void addSpectrum(const std::vector<float>& db, float floorDb, float ceilDb,
                     int sampleRate);
    void addSpectrumPoints(const std::vector<float>& points, float floorDb,
                           float ceilDb, int streamRate);
    void setYAxisUp(bool enabled) { yAxisUp = enabled; geometryDirty = true; }
    void setHistoryShift(float shiftX) { historyShiftX = shiftX; geometryDirty = true; }
    void clear();
    void releaseGL();
    size_t historySize() const { return history.size(); }

protected:
    void drawPanelContents() override;

private:
    static constexpr size_t HISTORY_BINS = 256;
    static constexpr size_t HISTORY_ROWS = 96;
    static constexpr int HISTORY_INTERVAL_MS = 40;

    std::deque<std::vector<float>> history;
    float floorValue = -100.0f;
    float ceilValue = 0.0f;
    int lastSampleRate = 0;
    bool yAxisUp = false;
    float historyShiftX = 0.0f;
    std::chrono::steady_clock::time_point lastAppend{};

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
#endif

    bool beginAppend(float floorDb, float ceilDb, int streamRate);
    void appendRow(std::vector<float>&& row);
    void rebuildGeometry();
};

// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#pragma once

#include "GLPanel.h"
#include <atomic>

class WaterfallPanel : public GLPanel {
public:
    WaterfallPanel();
    void setup(unsigned int fft_size_in, int num_waterfall_lines_in);
    void refreshTheme();
    void setPoints(std::vector<float> &points_in);
    void step();
    void update();
    void releaseGL();
    
protected:
    void drawPanelContents() override;
    
private:
    std::vector<float> points;

    GLuint waterfall[2];
    int waterfall_ofs[2];
    unsigned int fft_size;
    int waterfall_lines;
    std::vector<unsigned char> lineBuffer[2];
    std::vector<unsigned char> rLineBuffer[2];
    std::atomic_int lines_buffered;
    std::atomic_bool texInitialized, bufferInitialized;
    
    ColorTheme *activeTheme;
#ifdef __APPLE__
    GLuint paletteProgram = 0;
    GLuint paletteTexture = 0;
    GLint waterfallSampler = -1;
    GLint paletteSampler = -1;
    bool paletteShaderAttempted = false;
    bool usePaletteShader = false;

    void initializePaletteShader();
    void updatePaletteTexture();
#endif
};

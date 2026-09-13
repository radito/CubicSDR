// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "WaterfallPanel.h"

#include <algorithm>
#include <array>

#ifdef __APPLE__
namespace {
GLuint compileWaterfallShader(GLenum type, const char *source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
}
#endif

WaterfallPanel::WaterfallPanel() : GLPanel(), fft_size(0), waterfall_lines(0), activeTheme(nullptr) {
	setFillColor(RGBA4f(0,0,0));
    for (unsigned int & i : waterfall) {
        i = 0;
    }
}

void WaterfallPanel::setup(unsigned int fft_size_in, int num_waterfall_lines_in) {
    waterfall_lines = num_waterfall_lines_in;
    fft_size = fft_size_in;
    lines_buffered.store(0);
    
    if (points.size() != fft_size) {
        points.resize(fft_size);
    }
    
    texInitialized.store(false);
    bufferInitialized.store(false);
}

void WaterfallPanel::refreshTheme() {
#ifdef __APPLE__
    if (usePaletteShader) {
        updatePaletteTexture();
        return;
    }
#endif
    glEnable (GL_TEXTURE_2D);
    
    for (unsigned int i : waterfall) {
        glBindTexture(GL_TEXTURE_2D, i);
        
        glPixelTransferi(GL_MAP_COLOR, GL_TRUE);
        glPixelMapfv(GL_PIXEL_MAP_I_TO_R, 256, &(ThemeMgr::mgr.currentTheme->waterfallGradient.getRed())[0]);
        glPixelMapfv(GL_PIXEL_MAP_I_TO_G, 256, &(ThemeMgr::mgr.currentTheme->waterfallGradient.getGreen())[0]);
        glPixelMapfv(GL_PIXEL_MAP_I_TO_B, 256, &(ThemeMgr::mgr.currentTheme->waterfallGradient.getBlue())[0]);
    }
}

void WaterfallPanel::setPoints(std::vector<float> &points_in) {
    size_t halfPts = points_in.size() / 2;
    if (halfPts == fft_size) {
       
        for (unsigned int i = 0; i < fft_size; i++) {
            points[i] = points_in[i * 2 + 1];
        }
    } else {
        points.assign(points_in.begin(), points_in.end());
    }
}

void WaterfallPanel::step() {
    unsigned int half_fft_size = fft_size / 2;

    if (!bufferInitialized.load()) {
        bufferInitialized.store(true);
    }
    
    if (!texInitialized.load()) {
        return;
    }
    
    if (!points.empty() && points.size() == fft_size) {
        for (int j = 0; j < 2; j++) {
            const unsigned int line = lines_buffered.load();
            const unsigned int newBufSize = half_fft_size * (line + 1);
            if (lineBuffer[j].size() < newBufSize) {
                lineBuffer[j].resize(newBufSize);
                rLineBuffer[j].resize(newBufSize);
            }
            unsigned char *destination = lineBuffer[j].data() + half_fft_size * line;
            for (unsigned int i = 0, iMax = half_fft_size; i < iMax; i++) {
                float v = points[j * half_fft_size + i];
                
                float wv = v < 0 ? 0 : (v > 0.99 ? 0.99 : v);
                
                destination[i] = (unsigned char) floor(wv * 255.0);
            }
        }
        lines_buffered++;
    }
}

void WaterfallPanel::update() {
    unsigned int half_fft_size = fft_size / 2;
    
    if (!bufferInitialized.load()) {
        return;
    }
    
    if (!texInitialized.load()) {
#ifdef __APPLE__
        initializePaletteShader();
#endif
        for (int i = 0; i < 2; i++) {
            if (waterfall[i]) {
                glDeleteTextures(1, &waterfall[i]);
                waterfall[i] = 0;
            }
            
            waterfall_ofs[i] = waterfall_lines - 1;
        }

        glGenTextures(2, waterfall);
        
        unsigned char *waterfall_tex;
        
        //Creates 2x 2D textures into card memory.
        //of size half_fft_size * waterfall_lines, which can be BIG.
        //The limit of the size of Waterfall is the size of the maximum supported 2D texture 
        //by the graphic card. (half_fft_size * waterfall_lines, i.e DEFAULT_DEMOD_WATERFALL_LINES_NB * DEFAULT_FFT_SIZE/2)
        waterfall_tex = new unsigned char[half_fft_size * waterfall_lines];
        memset(waterfall_tex, 0, half_fft_size * waterfall_lines);
        
        for (unsigned int i : waterfall) {
            glBindTexture(GL_TEXTURE_2D, i);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            
#ifdef __APPLE__
            if (usePaletteShader) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE8,
                             half_fft_size, waterfall_lines, 0, GL_LUMINANCE,
                             GL_UNSIGNED_BYTE, (GLvoid *) waterfall_tex);
            } else
#endif
            {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, half_fft_size,
                             waterfall_lines, 0, GL_COLOR_INDEX,
                             GL_UNSIGNED_BYTE, (GLvoid *) waterfall_tex);
            }
        }
        
        delete[] waterfall_tex;

        refreshTheme();

        texInitialized.store(true);
    }
    
    for (int i = 0, iMax = lines_buffered.load(); i < iMax; i++) {
        for (int j = 0; j < 2; j++) {
            memcpy(&(rLineBuffer[j][i*half_fft_size]),
                   &(lineBuffer[j][((iMax-1)*half_fft_size)-(i*half_fft_size)]), sizeof(unsigned char) * half_fft_size);
        }
    }
    
    unsigned int run_ofs = 0;
    while (lines_buffered.load()) {
        int run_lines = lines_buffered.load();
        if (run_lines > waterfall_ofs[0]) {
            run_lines = waterfall_ofs[0];
        }
        for (int j = 0; j < 2; j++) {
            glBindTexture(GL_TEXTURE_2D, waterfall[j]);
#ifdef __APPLE__
            const GLenum inputFormat = usePaletteShader ? GL_LUMINANCE : GL_COLOR_INDEX;
#else
            const GLenum inputFormat = GL_COLOR_INDEX;
#endif
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, waterfall_ofs[j]-run_lines, half_fft_size, run_lines,
                            inputFormat, GL_UNSIGNED_BYTE, (GLvoid *) &(rLineBuffer[j][run_ofs]));
            
            waterfall_ofs[j]-=run_lines;
            
            if (waterfall_ofs[j] == 0) {
                waterfall_ofs[j] = waterfall_lines;
            }
        }
        run_ofs += run_lines*half_fft_size;
        lines_buffered.store(lines_buffered.load()-run_lines);
    }
}

void WaterfallPanel::drawPanelContents() {
    if (!texInitialized.load()) {
        return;
    }

    unsigned int half_fft_size = fft_size / 2;
    
    glLoadMatrixf(transform.to_ptr());
    
    glEnable (GL_TEXTURE_2D);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
    
    if (activeTheme != ThemeMgr::mgr.currentTheme) {
        refreshTheme();
        activeTheme = ThemeMgr::mgr.currentTheme;
    }
    glColor3f(1.0, 1.0, 1.0);
    
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    
    float viewWidth = (float) vp[2];
    
    // some bias to prevent seams at odd scales
    float half_pixel = 1.0 / viewWidth;
    float half_texel = 1.0 / (float) half_fft_size;
    float vtexel = 1.0 / (float) waterfall_lines;
    float vofs = (float) (waterfall_ofs[0]) * vtexel;

#ifdef __APPLE__
    if (usePaletteShader) {
        glUseProgram(paletteProgram);
        glActiveTexture(GL_TEXTURE1);
        glEnable(GL_TEXTURE_1D);
        glBindTexture(GL_TEXTURE_1D, paletteTexture);
        glActiveTexture(GL_TEXTURE0);
    }
#endif
    
    glBindTexture(GL_TEXTURE_2D, waterfall[0]);
    glBegin (GL_QUADS);
    glTexCoord2f(0.0 + half_texel, 1.0 + vofs);
    glVertex3f(-1.0, -1.0, 0.0);
    glTexCoord2f(1.0 - half_texel, 1.0 + vofs);
    glVertex3f(0.0 + half_pixel, -1.0, 0.0);
    glTexCoord2f(1.0 - half_texel, 0.0 + vofs);
    glVertex3f(0.0 + half_pixel, 1.0, 0.0);
    glTexCoord2f(0.0 + half_texel, 0.0 + vofs);
    glVertex3f(-1.0, 1.0, 0.0);
    glEnd();
    
    vofs = (float) (waterfall_ofs[1]) * vtexel;
    glBindTexture(GL_TEXTURE_2D, waterfall[1]);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0 + half_texel, 1.0 + vofs);
    glVertex3f(0.0 - half_pixel, -1.0, 0.0);
    glTexCoord2f(1.0 - half_texel, 1.0 + vofs);
    glVertex3f(1.0, -1.0, 0.0);
    glTexCoord2f(1.0 - half_texel, 0.0 + vofs);
    glVertex3f(1.0, 1.0, 0.0);
    glTexCoord2f(0.0 + half_texel, 0.0 + vofs);
    glVertex3f(0.0 - half_pixel, 1.0, 0.0);
    glEnd();
    
    glBindTexture(GL_TEXTURE_2D, 0);

#ifdef __APPLE__
    if (usePaletteShader) {
        glUseProgram(0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_1D, 0);
        glDisable(GL_TEXTURE_1D);
        glActiveTexture(GL_TEXTURE0);
    }
#endif
    
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDisable(GL_TEXTURE_2D);
}

void WaterfallPanel::releaseGL() {
    glDeleteTextures(2, waterfall);
    waterfall[0] = waterfall[1] = 0;
#ifdef __APPLE__
    if (paletteTexture) glDeleteTextures(1, &paletteTexture);
    if (paletteProgram) glDeleteProgram(paletteProgram);
    paletteTexture = 0;
    paletteProgram = 0;
#endif
}

#ifdef __APPLE__
void WaterfallPanel::initializePaletteShader() {
    if (paletteShaderAttempted) return;
    paletteShaderAttempted = true;

    static const char *vertexSource =
        "#version 120\n"
        "varying vec2 textureCoordinate;\n"
        "void main() {\n"
        "  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
        "  textureCoordinate = gl_MultiTexCoord0.xy;\n"
        "}\n";
    static const char *fragmentSource =
        "#version 120\n"
        "uniform sampler2D waterfallTexture;\n"
        "uniform sampler1D colorPalette;\n"
        "varying vec2 textureCoordinate;\n"
        "void main() {\n"
        "  float level = texture2D(waterfallTexture, textureCoordinate).r;\n"
        "  gl_FragColor = texture1D(colorPalette, level);\n"
        "}\n";

    const GLuint vertex = compileWaterfallShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compileWaterfallShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return;
    }

    paletteProgram = glCreateProgram();
    glAttachShader(paletteProgram, vertex);
    glAttachShader(paletteProgram, fragment);
    glLinkProgram(paletteProgram);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(paletteProgram, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        glDeleteProgram(paletteProgram);
        paletteProgram = 0;
        return;
    }

    waterfallSampler = glGetUniformLocation(paletteProgram, "waterfallTexture");
    paletteSampler = glGetUniformLocation(paletteProgram, "colorPalette");
    glUseProgram(paletteProgram);
    glUniform1i(waterfallSampler, 0);
    glUniform1i(paletteSampler, 1);
    glUseProgram(0);
    glGenTextures(1, &paletteTexture);
    usePaletteShader = paletteTexture != 0;
}

void WaterfallPanel::updatePaletteTexture() {
    if (!paletteTexture) return;
    constexpr size_t paletteSize = 256;
    std::array<unsigned char, paletteSize * 4> palette{};
    auto& gradient = ThemeMgr::mgr.currentTheme->waterfallGradient;
    const auto& red = gradient.getRed();
    const auto& green = gradient.getGreen();
    const auto& blue = gradient.getBlue();
    if (red.empty() || green.empty() || blue.empty()) return;

    for (size_t i = 0; i < paletteSize; ++i) {
        const size_t source = std::min(i, red.size() - 1);
        palette[i * 4] = static_cast<unsigned char>(
            std::clamp(red[source], 0.0f, 1.0f) * 255.0f);
        palette[i * 4 + 1] = static_cast<unsigned char>(
            std::clamp(green[std::min(i, green.size() - 1)], 0.0f, 1.0f) * 255.0f);
        palette[i * 4 + 2] = static_cast<unsigned char>(
            std::clamp(blue[std::min(i, blue.size() - 1)], 0.0f, 1.0f) * 255.0f);
        palette[i * 4 + 3] = 255;
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_1D, paletteTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA8, paletteSize, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, palette.data());
    glBindTexture(GL_TEXTURE_1D, 0);
    glActiveTexture(GL_TEXTURE0);
}
#endif

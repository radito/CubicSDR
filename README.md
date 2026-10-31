CubicSDR [![CircleCI](https://dl.circleci.com/status-badge/img/gh/cjcliffe/CubicSDR/tree/master.svg?style=svg)](https://dl.circleci.com/status-badge/redirect/gh/cjcliffe/CubicSDR/tree/master)
========

Cross-Platform Software-Defined Radio Application

- The latest downloads are available on the [CubicSDR Releases](https://github.com/cjcliffe/CubicSDR/releases) page.
- Build instructions can be found at the [CubicSDR Wiki](https://github.com/cjcliffe/CubicSDR/wiki) page.
- Manual is available at [cubicsdr.readthedocs.io](http://cubicsdr.readthedocs.io).
- Manual contributions can be submitted to the [CubicSDR-Manual](https://github.com/cjcliffe/CubicSDR-Manual) repository.

What's new in this fork
-----------------------

This fork is based on [cjcliffe/CubicSDR](https://github.com/cjcliffe/CubicSDR). Native macOS Apple Silicon (ARM64) DMGs are built by GitHub Actions and published on the [fork's Releases](https://github.com/radito/CubicSDR/releases) page.

Changes are grouped by area. **Type** marks whether a change is a new feature, an optimization or a fix.

| Area | Type | Change | Details |
|------|------|--------|---------|
| **RDS** | New | FM RDS decoding | Decodes PI, PS (station name), PTY, TP and TA on the FMS (FM stereo) demodulator. Shows sync state, resets on retune. |
| | New | RadioText | Decodes groups 2A/2B, handling the A/B flag and the end-of-text marker. Shows the complete text on a second line above the demodulator label. |
| | New | Mono-station RDS | Detects the 19 kHz pilot. When a station has no pilot, RDS is mixed with a fixed 57 kHz carrier, so mono stations still decode. |
| | Optimized | Weak-signal error correction | Corrects error bursts of up to 5 bits once locked. A station is confirmed by two consecutive groups with the same PI. A character from a corrected block is shown only after it repeats, so noise doesn't produce garbage text. |
| **Squelch** | New | NBFM noise-quality squelch ("Q" toggle) | Measures noise above the voice band against the open-channel level, so speech pauses, off-tune signals and CTCSS tones no longer close the squelch. Saved per demodulator. |
| | Fixed | Click-free gating | Smoothed level, pre-roll and fades. The hang time depends on why the squelch closes: 500 ms when the level dips but audio is clean, 60 ms on noise (tail about 120 ms). Decides on IQ power, not audio power. |
| **Audio** | New | RNNoise noise reduction | Selectable Off / Mid (legacy model, more sensitive on weak signals) / Strong (newer model). Correct 48 kHz framing and resampling. |
| | New | FM/NBFM de-emphasis | Configurable de-emphasis time constant. |
| | New | Playout diagnostics | Logs buffered latency, rate trim, dropouts, overflows and catch-ups. |
| | Optimized | Playout latency control | Trims the playback rate by at most ±500 ppm with a windowed-sinc interpolator (bit-exact when no trim is needed). Latency stays bounded instead of creeping up from SDR/sound-card clock drift. After a stall, the backlog is skipped instead of becoming permanent delay. |
| | Optimized | Lock-free stereo ring buffers | Replace queues and mutexes in the audio callback with atomic ring buffers. Steadier macOS playback. |
| **DSP & modems** | New | Symbol synchronization | Digital modems use symbol-timing recovery; FSK buffering improved. |
| | Optimized | Fewer copies | IQ blocks are swapped into the modem instead of copied. The FFT uses a circular buffer. Visual queues and buffer caches are bounded. |
| | Fixed | Stream-gap handling | Stream-gap metadata is propagated without guessing from timestamp jitter. Oscillator tuning and partial channelizer blocks survive resets and packet boundaries. |
| **Spectrum & graphics** | New | 3D spectrum history | 3D history view for demodulated audio, plus an optional overlay for the main FFT. Toggled from the button below peak hold. |
| | Optimized | VBO rendering | 3D history uses cached vertex buffers (three draw calls per frame). The waterfall uses compact palette-shaded textures. OpenGL contexts are released properly. |
| | Fixed | Correct FFT power | Hann window and normalized FFT power. Fixes NaN dB rendering and spectrum speed sync. |
| **SDR & threads** | Optimized | macOS QoS | SDR reader, post-processing, demodulator and audio threads all run at user-interactive QoS, avoiding priority inversion. |
| | Fixed | Runtime stability | Better SoapyRemote persistence, stream-timeout handling, disconnect recovery and shutdown, to prevent UI freezes and crashes. |
| **Build & tests** | New | Self-contained macOS app bundle | Bundled, relocated and validated dependencies. Native ARM64 build with `-O3 -mcpu=apple-m1` and LTO in CI. The DMG uses LZMA compression (about 18.6% smaller). |
| | New | `./build.sh` | One-command local macOS build and bundle using Homebrew dependencies (see [Building](#building)). |
| | New | Unit test suite | Hardware-free tests covering RDS (end-to-end through the FMS demodulator), NBFM squelch, audio playout simulation, modems, FFT and device capabilities. |

Building
--------

### macOS (Apple Silicon)

1. Install the Xcode Command Line Tools and the Homebrew dependencies:

       xcode-select --install
       brew install cmake wxwidgets@3.2 liquid-dsp soapysdr hamlib librtlsdr soapyrtlsdr

   `soapyrtlsdr` gives you RTL-SDR support. `build.sh` needs at least one SoapySDR device module installed. To use other hardware, install its module too, for example from the `pothosware/pothos` tap (`brew tap pothosware/pothos && brew install soapyhackrf soapyairspy`) or built from the [pothosware](https://github.com/pothosware) sources. Every installed module is bundled into the app.

2. Build a self-contained, ad-hoc signed app bundle:

       ./build.sh

   The app is written to `build-clean/dist/CubicSDR.app`.

Useful options:

| Command | Effect |
|---------|--------|
| `./build.sh --clean` | Delete the build directory and rebuild from scratch |
| `./build.sh --run` | Build, then launch the app |
| `./build.sh -- -DUSE_RNNOISE=OFF` | Pass extra CMake options after `--` |
| `BUILD_TYPE=Debug ./build.sh` | Debug build (default is `Release`) |
| `BUILD_DIR=build-dev ./build.sh` | Use another build directory (default is `build-clean`) |
| `JOBS=4 ./build.sh` | Limit parallel build jobs (default is the logical CPU count) |
| `WX_CONFIG=/path/to/wx-config ./build.sh` | Use a different wxWidgets 3.2 `wx-config` |

Release DMGs are built by the [macOS ARM64 DMG workflow](.github/workflows/macos-arm64-dmg.yml). It adds `-mcpu=apple-m1` tuning and LTO, builds pinned SoapySDR device modules, and packages an LZMA-compressed DMG.

### Linux and Windows

`build.sh` supports macOS only. On other platforms, configure and build with CMake directly, following the upstream [CubicSDR Wiki](https://github.com/cjcliffe/CubicSDR/wiki) build instructions:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel

Unit tests
----------

Tests are built by default through CMake's `BUILD_TESTING` option and do not require SDR hardware or a graphical session. After configuring and building, run:

    ctest --test-dir build-clean --output-on-failure

To build only the test executable:

    cmake --build build-clean --target CubicSDRTests

Clang builds can also generate a terminal summary and browsable HTML coverage report:

    cmake -S . -B build-coverage -DCUBICSDR_TEST_COVERAGE=ON
    cmake --build build-coverage --target CubicSDRCoverage

The HTML report is written to `build-coverage/tests/coverage/index.html`.
The coverage target requires at least 80% region, function, line, and branch
coverage. It measures code linked into `CubicSDRTests`, excluding test sources
and vendored code; it does not represent coverage of the entire GUI app or
live audio/SDR hardware paths. Device capability tests use an in-process fake
SoapySDR driver and do not connect to a receiver.

Utilizes: 
--------
  - liquid-dsp (http://liquidsdr.org/ -- https://github.com/jgaeddert/liquid-dsp)
  - SoapySDR (http://www.pothosware.com/ -- https://github.com/pothosware/SoapySDR)
  - RtAudio (http://www.music.mcgill.ca/~gary/rtaudio/ -- http://github.com/thestk/rtaudio/)
  - LodePNG (http://lodev.org/lodepng/)
  - BMFont (http://www.angelcode.com/ -- http://www.angelcode.com/products/bmfont/)
  - Bitstream Vera font (http://en.wikipedia.org/wiki/Bitstream_Vera)
  - OpenGL (https://www.opengl.org/)
  - wxWidgets (https://www.wxwidgets.org/)
  - CMake (http://www.cmake.org/)

Optional Libs:
--------
  - FFTW3 (can be compiled into liquid-dsp if desired) (http://www.fftw.org/ -- https://github.com/FFTW/fftw3)
  - hamlib (https://github.com/Hamlib/Hamlib)

Recommended minimum requirements:
--------------------
  - Multi-core processor system with at least 1GB RAM.
  - Graphics card with at least 128MB video memory and OpenGL 3.x or ES 2.0 support.
  - OSX 10.9+ for Mac binary releases.
  - Windows 7+ for 64 or 32-bit Windows binary releases.
  - Linux and other embedded distribution support yet to be indexed, known to at least work on Debian 8+ and Ubuntu 14+.

Target Platforms:
----------------
  - [x] OSX
  - [x] Windows
  - [x] Linux


License:
-------
  - GPL-2.0+

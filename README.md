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

Changes are grouped by area. **Type** marks whether a change is a new feature (green), an optimization (blue) or a fix (yellow).

| Area | Type | Change | Details |
|------|------|--------|---------|
| **RDS** | ![New](https://img.shields.io/badge/New-2ea44f?style=flat-square) | RDS decoding | PI, PS, PTY, TP, TA on the FMS demod |
| | ![New](https://img.shields.io/badge/New-2ea44f?style=flat-square) | RadioText | 2A/2B text shown above the demod label |
| | ![New](https://img.shields.io/badge/New-2ea44f?style=flat-square) | Mono-station RDS | Decodes stations without a 19 kHz pilot |
| | ![Optimized](https://img.shields.io/badge/Optimized-0969da?style=flat-square) | Error correction | 5-bit burst fix, no garbage on weak signals |
| **Squelch** | ![New](https://img.shields.io/badge/New-2ea44f?style=flat-square) | NBFM "Q" squelch | Noise-based, stays open in speech pauses |
| | ![Fixed](https://img.shields.io/badge/Fixed-d4a72c?style=flat-square) | Click-free gating | Pre-roll, fades, ~120 ms tail |
| **Audio** | ![New](https://img.shields.io/badge/New-2ea44f?style=flat-square) | RNNoise | Off / Mid / Strong noise reduction |
| | ![New](https://img.shields.io/badge/New-2ea44f?style=flat-square) | De-emphasis | Configurable for FM and NBFM |
| | ![New](https://img.shields.io/badge/New-2ea44f?style=flat-square) | Playout diagnostics | Logs latency, dropouts, overflows |
| | ![Optimized](https://img.shields.io/badge/Optimized-0969da?style=flat-square) | Latency control | Bounded delay despite clock drift and stalls |
| | ![Optimized](https://img.shields.io/badge/Optimized-0969da?style=flat-square) | Lock-free ring buffers | No mutexes in the audio callback |
| **DSP & modems** | ![New](https://img.shields.io/badge/New-2ea44f?style=flat-square) | Symbol sync | Timing recovery for digital modes |
| | ![Optimized](https://img.shields.io/badge/Optimized-0969da?style=flat-square) | Fewer copies | IQ swapped, circular FFT buffer |
| | ![Fixed](https://img.shields.io/badge/Fixed-d4a72c?style=flat-square) | Stream gaps | Tuning survives resets and packet edges |
| **Spectrum & graphics** | ![New](https://img.shields.io/badge/New-2ea44f?style=flat-square) | 3D spectrum history | Audio history view and FFT overlay |
| | ![Optimized](https://img.shields.io/badge/Optimized-0969da?style=flat-square) | VBO rendering | 3 draw calls per frame, compact waterfall |
| | ![Fixed](https://img.shields.io/badge/Fixed-d4a72c?style=flat-square) | FFT power | Hann window, normalized dB, no NaN |
| **SDR & threads** | ![Optimized](https://img.shields.io/badge/Optimized-0969da?style=flat-square) | macOS QoS | All DSP threads at user-interactive priority |
| | ![Fixed](https://img.shields.io/badge/Fixed-d4a72c?style=flat-square) | Runtime stability | SoapyRemote, timeouts, reconnect, shutdown |
| **Build & tests** | ![New](https://img.shields.io/badge/New-2ea44f?style=flat-square) | macOS app bundle | Native ARM64, LTO, LZMA DMG (~19% smaller) |
| | ![New](https://img.shields.io/badge/New-2ea44f?style=flat-square) | `./build.sh` | One-command build (see [Building](#building)) |
| | ![New](https://img.shields.io/badge/New-2ea44f?style=flat-square) | Unit tests | Hardware-free: RDS, squelch, audio, modems |

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

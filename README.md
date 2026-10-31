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
| **RDS** | New | RDS decoding | PI, PS, PTY, TP, TA on the FMS demod |
| | New | RadioText | 2A/2B text shown above the demod label |
| | New | Mono-station RDS | Decodes stations without a 19 kHz pilot |
| | Optimized | Error correction | 5-bit burst fix, no garbage on weak signals |
| **Squelch** | New | NBFM "Q" squelch | Noise-based, stays open in speech pauses |
| | Fixed | Click-free gating | Pre-roll, fades, ~120 ms tail |
| **Audio** | New | RNNoise | Off / Mid / Strong noise reduction |
| | New | De-emphasis | Configurable for FM and NBFM |
| | New | Playout diagnostics | Logs latency, dropouts, overflows |
| | Optimized | Latency control | Bounded delay despite clock drift and stalls |
| | Optimized | Lock-free ring buffers | No mutexes in the audio callback |
| **DSP & modems** | New | Symbol sync | Timing recovery for digital modes |
| | Optimized | Fewer copies | IQ swapped, circular FFT buffer |
| | Fixed | Stream gaps | Tuning survives resets and packet edges |
| **Spectrum & graphics** | New | 3D spectrum history | Audio history view and FFT overlay |
| | Optimized | VBO rendering | 3 draw calls per frame, compact waterfall |
| | Fixed | FFT power | Hann window, normalized dB, no NaN |
| **SDR & threads** | Optimized | macOS QoS | All DSP threads at user-interactive priority |
| | Fixed | Runtime stability | SoapyRemote, timeouts, reconnect, shutdown |
| **Build & tests** | New | macOS app bundle | Native ARM64, LTO, LZMA DMG (~19% smaller) |
| | New | `./build.sh` | One-command build (see [Building](#building)) |
| | New | Unit tests | Hardware-free: RDS, squelch, audio, modems |

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

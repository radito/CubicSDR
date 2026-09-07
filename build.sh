#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Build and bundle CubicSDR on macOS using Homebrew dependencies.

Usage: ./build.sh [--clean] [--run] [-- <extra CMake options>]

Environment variables:
  BUILD_DIR   Build directory (default: <repository>/build-clean)
  BUILD_TYPE  CMake build type (default: Release)
  JOBS        Number of parallel build jobs (default: logical CPU count)
  WX_CONFIG   Path to wx-config (default: Homebrew wx-config-3.2)

Examples:
  ./build.sh
  ./build.sh --clean
  ./build.sh --run
  ./build.sh -- -DUSE_RNNOISE=OFF
EOF
}

clean_build=0
run_app=0
cmake_extra_args=()

while (($#)); do
  case "$1" in
    --clean)
      clean_build=1
      ;;
    --run)
      run_app=1
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --)
      shift
      cmake_extra_args=("$@")
      break
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This build script currently supports macOS only." >&2
  exit 1
fi

for command_name in brew cmake codesign install_name_tool otool SoapySDRUtil xcode-select; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Missing required command: $command_name" >&2
    exit 1
  fi
done

if ! xcode-select -p >/dev/null 2>&1; then
  echo "Xcode Command Line Tools are required. Install them with: xcode-select --install" >&2
  exit 1
fi

missing_packages=()
for package_name in wxwidgets@3.2 liquid-dsp soapysdr hamlib librtlsdr; do
  if ! brew list --versions "$package_name" >/dev/null 2>&1; then
    missing_packages+=("$package_name")
  fi
done

if ((${#missing_packages[@]})); then
  echo "Missing Homebrew dependencies: ${missing_packages[*]}" >&2
  echo "Install them with: brew install ${missing_packages[*]}" >&2
  exit 1
fi

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${BUILD_DIR:-${source_dir}/build-clean}"
build_type="${BUILD_TYPE:-Release}"
jobs="${JOBS:-$(sysctl -n hw.logicalcpu)}"

if [[ "$build_dir" != /* ]]; then
  build_dir="${source_dir}/${build_dir}"
fi

homebrew_prefix="$(brew --prefix)"
liquid_prefix="$(brew --prefix liquid-dsp)"
soapy_prefix="$(brew --prefix soapysdr)"
wx_config="${WX_CONFIG:-${homebrew_prefix}/bin/wx-config-3.2}"
soapy_abi_version="$(SoapySDRUtil --info 2>&1 | sed -n 's/^[[:space:]]*ABI Version: v//p' | head -n 1)"
soapy_module_root="${homebrew_prefix}/lib/SoapySDR/modules${soapy_abi_version}"
soapy_module_dir="${soapy_module_root}/cubicsdr"

if [[ ! -d "$soapy_module_dir" ]]; then
  soapy_module_dir="$soapy_module_root"
fi

if [[ ! -x "$wx_config" ]]; then
  echo "wx-config was not found at: $wx_config" >&2
  echo "Set WX_CONFIG to the correct wx-config 3.2 executable." >&2
  exit 1
fi

if [[ -z "$soapy_abi_version" || ! -d "$soapy_module_dir" ]]; then
  echo "No Homebrew SoapySDR module directory was found." >&2
  echo "Install at least one SoapySDR hardware support module before building." >&2
  exit 1
fi

if ((clean_build)); then
  case "$build_dir" in
    "${source_dir}"/build*) rm -rf -- "$build_dir" ;;
    *)
      echo "Refusing to clean unexpected build directory: $build_dir" >&2
      exit 1
      ;;
  esac
fi

cmake_args=(
  -S "$source_dir"
  -B "$build_dir"
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  -DCMAKE_BUILD_TYPE="$build_type"
  -DCMAKE_INSTALL_PREFIX="$homebrew_prefix"
  -DCMAKE_PREFIX_PATH="$homebrew_prefix"
  -DLIQUID_INCLUDES="$liquid_prefix/include"
  -DLIQUID_LIBRARIES="$liquid_prefix/lib/libliquid.dylib"
  -DSoapySDR_DIR="$soapy_prefix/share/cmake/SoapySDR"
  -DwxWidgets_CONFIG_EXECUTABLE="$wx_config"
  -DBUNDLE_APP=ON
  -DCUBICSDR_CODE_SIGN=OFF
  -DBUNDLE_SOAPY_MODS=ON
  -DBUNDLED_MODS_ONLY=ON
  -DUSE_HAMLIB=ON
  -DENABLE_DIGITAL_LAB=ON
)

if ((${#cmake_extra_args[@]})); then
  cmake_args+=("${cmake_extra_args[@]}")
fi

cmake "${cmake_args[@]}"

cmake --build "$build_dir" --parallel "$jobs"

raw_app_path="${build_dir}/x64/CubicSDR.app"
app_path="${build_dir}/dist/CubicSDR.app"
module_destination="${app_path}/Contents/MacOS/modules"

rm -rf -- "$app_path"
cmake -E make_directory "$(dirname "$app_path")"
cmake -E copy_directory "$raw_app_path" "$app_path"
cmake -E make_directory "$module_destination"

shopt -s nullglob
soapy_modules=("$soapy_module_dir"/*.so)
shopt -u nullglob

if ((${#soapy_modules[@]} == 0)); then
  echo "No SoapySDR modules were found in: $soapy_module_dir" >&2
  exit 1
fi

for soapy_module in "${soapy_modules[@]}"; do
  cmake -E copy_if_different "$soapy_module" "$module_destination/"
done

# Some locally built SoapyRTLSDR modules retain the old Homebrew major-name
# dependency even though the installed library now uses librtlsdr.0.dylib.
# Repair only the copied module; never modify the Homebrew installation.
rtl_module="${module_destination}/librtlsdrSupport.so"
rtl_library="$(brew --prefix librtlsdr)/lib/librtlsdr.0.dylib"
if [[ -f "$rtl_module" && -f "$rtl_library" ]]; then
  rtl_module_dependency="$(otool -L "$rtl_module" | sed -n '/\/librtlsdr\.[0-9][^ ]*\.dylib/{s/^[[:space:]]*//;s/[[:space:]].*$//;p;q;}')"
  if [[ -n "$rtl_module_dependency" && ! -e "$rtl_module_dependency" ]]; then
    install_name_tool -change "$rtl_module_dependency" "$rtl_library" "$rtl_module"
  fi
fi

cmake \
  -DAPP_BUNDLE="$app_path" \
  -DDEPENDENCY_DIRS="${homebrew_prefix}/lib;/usr/local/lib" \
  -P "${source_dir}/cmake/BundleMacOS.cmake"

codesign --force --deep --sign - "$app_path"
codesign --verify --deep --strict "$app_path"

echo "Built self-contained CubicSDR: $app_path"

if ((run_app)); then
  open "$app_path"
fi

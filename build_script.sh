#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

error() { echo -e "\033[31m[ERROR]\033[0m $*" >&2; }
info()  { echo -e "\033[36m[INFO]\033[0m $*"; }
ok()    { echo -e "\033[32m[OK]\033[0m $*"; }

# -- Help -------------------------------------------------------------------
show_help() {
    cat <<'EOF'
Usage: build_script.sh [options]

Options:
  -c, --clean    Remove build directory before building
  -r, --rebuild  Full clean rebuild (same as -c)
  -d, --debug    Build with Debug config (default: Release)
  -j N           Use N parallel jobs (default: nproc)
  -h, --help     Show this help
EOF
    exit 0
}

# -- Parse flags -------------------------------------------------------------
CLEAN=0
BUILD_TYPE="Release"
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--clean|-r|--rebuild) CLEAN=1; shift ;;
        -d|--debug) BUILD_TYPE="Debug"; shift ;;
        -j) JOBS="$2"; shift 2 ;;
        -h|--help) show_help ;;
        *) error "Unknown option: $1"; show_help ;;
    esac
done

# -- Dependency checks -------------------------------------------------------
info "Checking required tools and libraries..."

DEPS_OK=1

check_cmd() {
    if ! command -v "$1" &>/dev/null; then
        error "$1 is not installed."
        DEPS_OK=0
        return 1
    fi
    local ver
    ver=$("$1" --version 2>&1 | head -n 1)
    ok "$1 found: $ver"
}

check_cmd cmake
check_cmd g++ || check_cmd clang++
check_cmd make

if command -v pkg-config &>/dev/null; then
    ok "pkg-config found"
else
    error "pkg-config is not installed."
    DEPS_OK=0
fi

# Check dev libraries via pkg-config
for dep in "x11:x11" "gl:gl" "xrandr:xrandr" "xi:xi" "xinerama:xinerama" "xcursor:xcursor"; do
    mod="${dep%%:*}"
    pkg="${dep##*:}"
    if ! pkg-config --exists "$pkg" 2>/dev/null; then
        error "$mod not found (pkg-config: $pkg). Install: lib${pkg}-dev"
        DEPS_OK=0
    else
        ok "$mod found"
    fi
done

# FFTW bundled
if [[ -d "${PROJECT_DIR}/fftw-3.3.10" ]]; then
    ok "FFTW source bundle found"
else
    error "FFTW source bundle missing at fftw-3.3.10/"
    DEPS_OK=0
fi

if [[ $DEPS_OK -ne 1 ]]; then
    error "Some dependencies are missing. Install them and try again."
    exit 1
fi

# -- Prepare build directory -------------------------------------------------
if [[ $CLEAN -eq 1 ]] && [[ -d "$BUILD_DIR" ]]; then
    info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

if [[ ! -d "$BUILD_DIR" ]]; then
    info "Creating build directory at $BUILD_DIR ..."
    mkdir -p "$BUILD_DIR"
fi

# -- Configure ---------------------------------------------------------------
info "Configuring with CMake (${BUILD_TYPE})..."
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5

# -- Generate asset headers -------------------------------------------------
info "Generating asset headers..."
mkdir -p "${BUILD_DIR}/generated"
(cd "${PROJECT_DIR}" && xxd -i assets/interferogram_curve.png) > \
    "${BUILD_DIR}/generated/interferogram_curve.h"

# -- Build -------------------------------------------------------------------
info "Building with ${JOBS} parallel job(s) ..."
cmake --build "$BUILD_DIR" -j "$JOBS"

echo ""
ok "Build successful! Binary at: ${BUILD_DIR}/fts_data_explorer"

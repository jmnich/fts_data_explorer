#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

error() { echo -e "\033[31m[ERROR]\033[0m $*" >&2; }
info()  { echo -e "\033[36m[INFO]\033[0m $*"; }
ok()    { echo -e "\033[32m[OK]\033[0m $*"; }

show_help() {
    cat <<'EOF'
Usage: build_script.sh [options]

Options:
  -c, --clean     Remove build directory before building
  -d, --debug     Build with Debug config (default: Release)
  -w, --windows   Cross-compile for Windows (MinGW)
  -j N            Use N parallel jobs (default: nproc)
  -h, --help      Show this help
EOF
    exit 0
}

# -- Parse flags -------------------------------------------------------------
CLEAN=0
BUILD_TYPE="Release"
TARGET="linux"
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--clean|-r|--rebuild) CLEAN=1; shift ;;
        -d|--debug) BUILD_TYPE="Debug"; shift ;;
        -w|--windows) TARGET="windows"; shift ;;
        -j) JOBS="$2"; shift 2 ;;
        -h|--help) show_help ;;
        *) error "Unknown option: $1"; show_help ;;
    esac
done

# -- Select preset -----------------------------------------------------------
if [[ "$TARGET" == "windows" ]]; then
    PRESET="windows-mingw"
    BUILD_DIR="${PROJECT_DIR}/build/windows-mingw"
elif [[ "$BUILD_TYPE" == "Debug" ]]; then
    PRESET="linux-debug"
    BUILD_DIR="${PROJECT_DIR}/build/linux-debug"
else
    PRESET="linux-release"
    BUILD_DIR="${PROJECT_DIR}/build/linux-release"
fi

# -- Dependency checks -------------------------------------------------------
info "Checking required tools..."

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

if [[ "$TARGET" == "windows" ]]; then
    if command -v x86_64-w64-mingw32-g++ &>/dev/null; then
        ok "MinGW cross-compiler found"
    else
        error "MinGW cross-compiler not found. Install: sudo apt install mingw-w64"
        DEPS_OK=0
    fi
else
    if ! command -v pkg-config &>/dev/null; then
        error "pkg-config is not installed. Install: pkg-config"
        DEPS_OK=0
    fi
fi

if [[ $DEPS_OK -ne 1 ]]; then
    error "Some dependencies are missing."
    exit 1
fi

# -- Clean -------------------------------------------------------------------
if [[ $CLEAN -eq 1 ]] && [[ -d "$BUILD_DIR" ]]; then
    info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# -- Build -------------------------------------------------------------------
info "Configuring with CMake (preset: ${PRESET})..."
cmake --preset "$PRESET"

info "Building with ${JOBS} parallel job(s) ..."
cmake --build --preset "$PRESET" -j "$JOBS"

echo ""
BINARY_NAME="fts_data_explorer"
[[ "$TARGET" == "windows" ]] && BINARY_NAME+=".exe"
ok "Build successful! Binary at: ${BUILD_DIR}/${BINARY_NAME}"

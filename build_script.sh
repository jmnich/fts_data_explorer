#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

error() { echo -e "\033[31m[ERROR]\033[0m $*" >&2; }
info()  { echo -e "\033[36m[INFO]\033[0m $*"; }
ok()    { echo -e "\033[32m[OK]\033[0m $*"; }

# ── Version helpers ─────────────────────────────────────────────────────────
VERSION_FILE="${PROJECT_DIR}/VERSION"

read_version() {
    local file="$1"
    if [[ ! -f "$file" ]]; then
        echo "0 0 0"
        return
    fi
    local raw
    raw=$(head -n 1 "$file" | tr -d '[:space:]')
    IFS='.' read -r yy mm minor <<< "$raw"
    echo "${yy:-0} ${mm:-0} ${minor:-0}"
}

write_version() {
    local yy="$1" mm="$2" minor="$3"
    echo "${yy}.${mm}.${minor}" > "$VERSION_FILE"
    ok "VERSION updated to ${yy}.${mm}.${minor}"
}

generate_version_header() {
    local version_string="$1" is_release="$2"
    sed "s|@APP_VERSION_VALUE@|${version_string}|g; s|@APP_IS_RELEASE_VALUE@|${is_release}|g" \
        "${PROJECT_DIR}/version.h.in" > "${PROJECT_DIR}/version.h"
    ok "Generated version.h: ${version_string}"
}

do_build() {
    local preset="$1" build_dir="$2" binary_name="$3"
    if [[ $CLEAN -eq 1 ]] && [[ -d "$build_dir" ]]; then
        info "Cleaning build directory ${build_dir}..."
        rm -rf "$build_dir"
    fi
    info "Configuring with CMake (preset: ${preset})..."
    cmake --preset "$preset"
    info "Building with ${JOBS} parallel job(s) ..."
    cmake --build --preset "$preset" -j "$JOBS"
    echo ""
    ok "Build successful! Binary at: ${build_dir}/${binary_name}"
}

# ── Help ────────────────────────────────────────────────────────────────────
show_help() {
    cat <<'EOF'
Usage: build_script.sh [options]

Options:
  -r, --release   Create a release build (bumps version, builds both
                  Linux and Windows, no "(dev)" suffix)
  -c, --clean     Remove build directory before building
  -d, --debug     Build with Debug config (default: Release)
  -w, --windows   Cross-compile for Windows (MinGW) — without -r,
                  builds a dev version for Windows only
  -j N            Use N parallel jobs (default: nproc)
  -h, --help      Show this help
EOF
    exit 0
}

# ── Parse flags ─────────────────────────────────────────────────────────────
CLEAN=0
BUILD_TYPE="Release"
TARGET="linux"
IS_RELEASE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--clean|-r|--rebuild) CLEAN=1; shift ;;
        -d|--debug) BUILD_TYPE="Debug"; shift ;;
        -w|--windows) TARGET="windows"; shift ;;
        -j) JOBS="$2"; shift 2 ;;
        -r|-release|--release) IS_RELEASE=1; TARGET="linux"; shift ;;
        -h|--help) show_help ;;
        *) error "Unknown option: $1"; show_help ;;
    esac
done

# If -release was given, ignore -w (sequential build handles both)
# If -release and -w are both given, -release takes precedence (does both)
if [[ $IS_RELEASE -eq 1 ]] && [[ "$TARGET" == "windows" ]]; then
    TARGET="linux"  # will build both
fi

# ── Version computation ─────────────────────────────────────────────────────
read -r stored_yy stored_mm stored_minor <<< "$(read_version "$VERSION_FILE")"

if [[ $IS_RELEASE -eq 1 ]]; then
    # Date-aware version computation
    current_yy=$(date +%y)
    current_mm=$(date +%m)
    current_minor=$((10#${current_mm}))
    current_yy=$((10#${current_yy}))

    if [[ "${current_yy}" -eq "${stored_yy}" && "${current_mm}" -eq "${stored_mm}" ]]; then
        new_minor=$(( stored_minor + 1 ))
    else
        new_minor=0
    fi

    # Format with leading zeros: YY.MM.MINOR
    printf -v version_yy   "%02d" "${current_yy}"
    printf -v version_mm   "%02d" "${current_mm}"
    printf -v version_minor "%d"  "${new_minor}"

    VERSION_STRING="${version_yy}.${version_mm}.${version_minor}"
    info "Release build: version = ${VERSION_STRING}"
else
    # Dev build: show stored version with "(dev)" suffix
    VERSION_STRING="${stored_yy}.${stored_mm}.${stored_minor} (dev)"
    info "Dev build: version = ${VERSION_STRING}"
fi

# ── Generate version.h ──────────────────────────────────────────────────────
generate_version_header "${VERSION_STRING}" "${IS_RELEASE}"

# ── Dependency checks ───────────────────────────────────────────────────────
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

# Cross-compiler check (only needed for -release which builds both)
if [[ $IS_RELEASE -eq 1 ]]; then
    if command -v x86_64-w64-mingw32-g++ &>/dev/null; then
        ok "MinGW cross-compiler found"
    else
        error "MinGW cross-compiler not found. Install: sudo apt install mingw-w64"
        error "Cannot build Windows binary without MinGW. Linux binary will still be built."
        HAS_MINGW=0
    fi
fi

if [[ "$TARGET" == "windows" ]]; then
    if command -v x86_64-w64-mingw32-g++ &>/dev/null; then
        ok "MinGW cross-compiler found"
    else
        error "MinGW cross-compiler not found. Install: sudo apt install mingw-w64"
        DEPS_OK=0
    fi
else
    # Only check pkg-config if not also building Windows (cross-compile uses its own sysroot)
    if [[ $IS_RELEASE -eq 0 ]]; then
        if ! command -v pkg-config &>/dev/null; then
            error "pkg-config is not installed. Install: pkg-config"
            DEPS_OK=0
        fi
    fi
fi

if [[ $DEPS_OK -ne 1 ]]; then
    error "Some dependencies are missing."
    exit 1
fi

# ── Build ────────────────────────────────────────────────────────────────────

if [[ $IS_RELEASE -eq 1 ]]; then
    # ── Release build: Linux + Windows sequentially ─────────────────────────
    LINUX_PRESET="linux-release"
    LINUX_BUILD_DIR="${PROJECT_DIR}/build/linux-release"
    LINUX_BINARY="fts_data_explorer"

    do_build "$LINUX_PRESET" "$LINUX_BUILD_DIR" "$LINUX_BINARY"

    # Copy Linux artifact to release_artifacts
    RELEASE_DIR="${PROJECT_DIR}/build/release_artifacts"
    mkdir -p "$RELEASE_DIR"
    cp "${LINUX_BUILD_DIR}/${LINUX_BINARY}" \
       "${RELEASE_DIR}/${LINUX_BINARY}"
    ok "Linux artifact: ${RELEASE_DIR}/${LINUX_BINARY}"

    # Build Windows
    if command -v x86_64-w64-mingw32-g++ &>/dev/null; then
        WIN_PRESET="windows-mingw"
        WIN_BUILD_DIR="${PROJECT_DIR}/build/windows-mingw"
        WIN_BINARY="fts_data_explorer.exe"

        do_build "$WIN_PRESET" "$WIN_BUILD_DIR" "$WIN_BINARY"

        cp "${WIN_BUILD_DIR}/${WIN_BINARY}" \
           "${RELEASE_DIR}/${WIN_BINARY}"
        ok "Windows artifact: ${RELEASE_DIR}/${WIN_BINARY}"
    else
        error "Skipping Windows build (MinGW not available)"
    fi

    # Update VERSION file (write the released version)
    write_version "${version_yy}" "${version_mm}" "${version_minor}"

elif [[ "$TARGET" == "windows" ]]; then
    # ── Standalone Windows dev build ────────────────────────────────────────
    PRESET="windows-mingw"
    BUILD_DIR="${PROJECT_DIR}/build/windows-mingw"
    BINARY_NAME="fts_data_explorer.exe"
    do_build "$PRESET" "$BUILD_DIR" "$BINARY_NAME"
else
    # ── Linux dev build ─────────────────────────────────────────────────────
    if [[ "$BUILD_TYPE" == "Debug" ]]; then
        PRESET="linux-debug"
        BUILD_DIR="${PROJECT_DIR}/build/linux-debug"
    else
        PRESET="linux-release"
        BUILD_DIR="${PROJECT_DIR}/build/linux-release"
    fi
    BINARY_NAME="fts_data_explorer"
    do_build "$PRESET" "$BUILD_DIR" "$BINARY_NAME"
fi

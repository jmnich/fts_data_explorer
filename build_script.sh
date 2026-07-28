#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

error() { echo -e "\033[31m[ERROR]\033[0m $*" >&2; }
warn()  { echo -e "\033[33m[WARN]\033[0m  $*" >&2; }
info()  { echo -e "\033[36m[INFO]\033[0m  $*"; }
ok()    { echo -e "\033[32m[OK]\033[0m    $*"; }

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
    ok "VERSION → ${yy}.${mm}.${minor}"
}

generate_version_header() {
    local version_string="$1" is_release="$2"
    sed "s|@APP_VERSION_VALUE@|${version_string}|g; s|@APP_IS_RELEASE_VALUE@|${is_release}|g" \
        "${PROJECT_DIR}/version.h.in" > "${PROJECT_DIR}/version.h"
}

do_build() {
    local preset="$1" build_dir="$2" binary_name="$3"

    # Clean
    if [[ $CLEAN -eq 1 ]] && [[ -d "$build_dir" ]]; then
        info "Removing ${build_dir} ..."
        rm -rf "$build_dir"
    fi

    # Configure (only when needed — cmake re-runs itself if CMakeLists.txt changes)
    if [[ ! -d "$build_dir" ]] || [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
        info "Configuring (preset: ${preset}) ..."
        cmake --preset "$preset"
    else
        info "Build tree exists, skipping configure."
    fi

    # Build
    info "Building (${JOBS} jobs) ..."
    cmake --build --preset "$preset" -j "$JOBS"

    ok "Binary: ${build_dir}/${binary_name}"
}

# ── Help ────────────────────────────────────────────────────────────────────
show_help() {
    cat <<'EOF'
Usage: build_script.sh [options]

Options:
  -r, --release   Create a release build (bumps version, builds Linux + Windows)
  -c, --clean     Remove build directory before building
  -d, --debug     Build with Debug config (default: Release)
  -w, --windows   Cross-compile for Windows (MinGW) — dev build only
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
        -r|--release)          IS_RELEASE=1; TARGET="linux"; shift ;;
        -c|--clean|--rebuild)  CLEAN=1; shift ;;
        -d|--debug)            BUILD_TYPE="Debug"; shift ;;
        -w|--windows)          TARGET="windows"; shift ;;
        -j)                    JOBS="$2"; shift 2 ;;
        -h|--help)             show_help ;;
        *)                     error "Unknown option: $1"; show_help ;;
    esac
done

# ── Version computation ─────────────────────────────────────────────────────
read -r stored_yy stored_mm stored_minor <<< "$(read_version "$VERSION_FILE")"

if [[ $IS_RELEASE -eq 1 ]]; then
    current_yy=$(date +%y)
    current_mm=$(date +%m)
    current_minor=$((10#${current_mm}))
    current_yy=$((10#${current_yy}))

    if [[ "${current_yy}" -eq "${stored_yy}" && "${current_mm}" -eq "${stored_mm}" ]]; then
        new_minor=$(( stored_minor + 1 ))
    else
        new_minor=0
    fi

    printf -v version_yy   "%02d" "${current_yy}"
    printf -v version_mm   "%02d" "${current_mm}"
    printf -v version_minor "%d"  "${new_minor}"

    VERSION_STRING="${version_yy}.${version_mm}.${version_minor}"
else
    VERSION_STRING="${stored_yy}.${stored_mm}.${stored_minor} (dev)"
fi

# ── Generate version header ─────────────────────────────────────────────────
generate_version_header "${VERSION_STRING}" "${IS_RELEASE}"
ok "version = ${VERSION_STRING}"

# ── Dependency checks ───────────────────────────────────────────────────────
info "Checking required tools ..."

MISSING=()
check_cmd() {
    if command -v "$1" &>/dev/null; then
        return 0
    fi
    MISSING+=("$1")
    return 1
}

check_cmd cmake    || true
check_cmd g++      || check_cmd clang++ || true
check_cmd make     || true

if [[ $TARGET == "windows" ]] || [[ $IS_RELEASE -eq 1 ]]; then
    check_cmd x86_64-w64-mingw32-g++ || true
fi
if [[ $TARGET != "windows" ]] && [[ $IS_RELEASE -eq 0 ]]; then
    check_cmd pkg-config || true
fi

if [[ ${#MISSING[@]} -gt 0 ]]; then
    error "Missing: ${MISSING[*]}"
    exit 1
fi
ok "All required tools present."

# ── Build ───────────────────────────────────────────────────────────────────

if [[ $IS_RELEASE -eq 1 ]]; then
    # ── Release: Linux + Windows sequential ─────────────────────────────────
    LINUX_PRESET="linux-release"
    LINUX_BUILD_DIR="${PROJECT_DIR}/build/linux-release"
    LINUX_BINARY="fts_data_explorer"

    do_build "$LINUX_PRESET" "$LINUX_BUILD_DIR" "$LINUX_BINARY"

    RELEASE_DIR="${PROJECT_DIR}/build/release_artifacts"
    mkdir -p "$RELEASE_DIR"
    cp "${LINUX_BUILD_DIR}/${LINUX_BINARY}" "${RELEASE_DIR}/${LINUX_BINARY}"
    ok "Release artifact: ${RELEASE_DIR}/${LINUX_BINARY}"

    # Windows cross-compile
    if command -v x86_64-w64-mingw32-g++ &>/dev/null; then
        WIN_PRESET="windows-mingw"
        WIN_BUILD_DIR="${PROJECT_DIR}/build/windows-mingw"
        WIN_BINARY="fts_data_explorer.exe"

        do_build "$WIN_PRESET" "$WIN_BUILD_DIR" "$WIN_BINARY"

        cp "${WIN_BUILD_DIR}/${WIN_BINARY}" "${RELEASE_DIR}/${WIN_BINARY}"
        ok "Release artifact: ${RELEASE_DIR}/${WIN_BINARY}"
    else
        warn "MinGW not found — skipping Windows build."
    fi

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

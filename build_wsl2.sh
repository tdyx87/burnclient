#!/usr/bin/env bash
# WSL2 build script for BurningDLL
# Usage: wsl -d Ubuntu bash build_wsl2.sh [build_type] [clean]
#   build_type: Debug | Release | RelWithDebInfo | MinSizeRel (default: Release)
#   clean:      pass "clean" to wipe build dir first

set -euo pipefail

BUILD_TYPE="${1:-Release}"
CLEAN="${2:-}"

# Resolve project dir (script must be run from project root or via wsl)
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build_linux"

echo "============================================"
echo " BurningDLL — WSL2 Build"
echo " Project: ${PROJECT_DIR}"
echo " Build type: ${BUILD_TYPE}"
echo " Build dir: ${BUILD_DIR}"
echo "============================================"

# Check dependencies
check_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "[ERROR] Missing tool: $1"
        return 1
    fi
}

echo "[1/5] Checking dependencies..."
check_tool cmake
check_tool make
check_tool g++

# Optional tools for ISO/burning
echo "  Optional tools:"
for t in xorriso growisofs wodim mkisofs genisoimage; do
    if command -v "$t" >/dev/null 2>&1; then
        echo "    $t: $(command -v $t)"
    else
        echo "    $t: not found (optional)"
    fi
done

# Clean if requested
if [ "${CLEAN}" = "clean" ]; then
    echo "[2/5] Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Configure
echo "[3/5] Configuring with CMake..."
cmake "${PROJECT_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_COMPILER=gcc

# Build
echo "[4/5] Building..."
make -j"$(nproc)"

echo "[5/5] Build complete!"
echo ""
echo "Outputs:"
find "${BUILD_DIR}" -name "libburning_dll*.so*" -o -name "test_burning_utility" 2>/dev/null | while read -r f; do
    echo "  $f"
done
echo ""
echo "To run tests:"
echo "  cd ${BUILD_DIR} && ctest --output-on-failure"
echo "  or: ${BUILD_DIR}/bin/test_burning_utility"

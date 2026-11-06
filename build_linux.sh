#!/bin/bash
# build_linux.sh - Build script for Linux

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build_linux"

echo "=== Building Burning Utility for Linux ==="

# Clean previous build
if [ -d "$BUILD_DIR" ]; then
    echo "Cleaning previous build..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure
cmake "$SCRIPT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_CXX_STANDARD=17

# Build
cmake --build . --parallel "$(nproc)"

echo ""
echo "=== Build Complete ==="
echo "Library: $BUILD_DIR/lib/libburning_dll.so"
echo "Test:    $BUILD_DIR/bin/test_burning_utility"
echo "CLI:     $BUILD_DIR/bin/burn_cli"
echo ""
echo "=== Running Tests ==="
./bin/test_burning_utility

echo ""
echo "=== CLI Usage ==="
echo "./bin/burn_cli list-devices"
echo "./bin/burn_cli platform-info"
echo "./bin/burn_cli create-iso <src_dir> <output.iso> [label]"

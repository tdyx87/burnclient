#!/bin/bash
# build_burning_tools.sh - Compile burning tools from source (not system packages)
# Run this on Linux to build genisoimage, wodim, growisofs, xorriso, mkisofs from source

set -e

INSTALL_PREFIX="${INSTALL_PREFIX:-$HOME/.local/burning_tools}"
BUILD_DIR="${BUILD_DIR:-/tmp/burning_tools_build}"

echo "=== Building Burning Tools from Source ==="
echo "Install prefix: $INSTALL_PREFIX"
echo "Build dir: $BUILD_DIR"

mkdir -p "$INSTALL_PREFIX/bin" "$BUILD_DIR"
cd "$BUILD_DIR"

# --- xorriso (from GNU xorriso) ---
echo ""
echo "=== Building xorriso ==="
XORRISO_VER="1.5.6"
if [ ! -f "xorriso-${XORRISO_VER}.tar.gz" ]; then
    wget "https://www.gnu.org/software/xorriso/xorriso-${XORRISO_VER}.tar.gz" || \
    curl -LO "https://www.gnu.org/software/xorriso/xorriso-${XORRISO_VER}.tar.gz"
fi
rm -rf "xorriso-${XORRISO_VER}"
tar xzf "xorriso-${XORRISO_VER}.tar.gz"
cd "xorriso-${XORRISO_VER}"
./configure --prefix="$INSTALL_PREFIX"
make -j"$(nproc)"
make install
cd "$BUILD_DIR"
echo "xorriso built: $INSTALL_PREFIX/bin/xorriso"

# --- dvd+rw-tools (growisofs) ---
echo ""
echo "=== Building growisofs (dvd+rw-tools) ==="
DVD_RW_VER="7.1"
if [ ! -f "dvd+rw-tools-${DVD_RW_VER}.tar.gz" ]; then
    wget "https://fy.chalmers.se/~appro/linux/DVD+RW/tools/dvd+rw-tools-${DVD_RW_VER}.tar.gz" || \
    curl -LO "https://fy.chalmers.se/~appro/linux/DVD+RW/tools/dvd+rw-tools-${DVD_RW_VER}.tar.gz"
fi
rm -rf "dvd+rw-tools-${DVD_RW_VER}"
tar xzf "dvd+rw-tools-${DVD_RW_VER}.tar.gz"
cd "dvd+rw-tools-${DVD_RW_VER}"
make
cp growisofs "$INSTALL_PREFIX/bin/"
cd "$BUILD_DIR"
echo "growisofs built: $INSTALL_PREFIX/bin/growisofs"

# --- cdrkit (genisoimage, wodim, mkisofs) ---
echo ""
echo "=== Building cdrkit (genisoimage, wodim, mkisofs) ==="
CDRKIT_VER="1.1.11"
if [ ! -f "cdrkit-${CDRKIT_VER}.tar.gz" ]; then
    wget "https://deb.debian.org/debian/pool/main/c/cdrkit/cdrkit_${CDRKIT_VER}.orig.tar.gz" -O "cdrkit-${CDRKIT_VER}.tar.gz" || \
    curl -L "https://deb.debian.org/debian/pool/main/c/cdrkit/cdrkit_${CDRKIT_VER}.orig.tar.gz" -o "cdrkit-${CDRKIT_VER}.tar.gz"
fi
rm -rf "cdrkit-${CDRKIT_VER}"
tar xzf "cdrkit-${CDRKIT_VER}.tar.gz"
cd "cdrkit-${CDRKIT_VER}"
mkdir -p build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"
make install
cd "$BUILD_DIR"
echo "genisoimage/wodim/mkisofs built in $INSTALL_PREFIX/bin/"

# --- Summary ---
echo ""
echo "=== All Burning Tools Built ==="
ls -la "$INSTALL_PREFIX/bin/"
echo ""
echo "Add to PATH:"
echo "  export PATH=\"$INSTALL_PREFIX/bin:\$PATH\""
echo ""
echo "Or set in your environment:"
echo "  export BURNING_TOOLS_PATH=\"$INSTALL_PREFIX/bin\""

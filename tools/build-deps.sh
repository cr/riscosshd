#!/bin/sh
# Cross-build mbedTLS and libssh as static libraries for RISC OS into
# deps/sysroot.  Runs inside the GCCSDK container.  See docs/toolchain.md.
set -e
cd "$(dirname "$0")/.."
TOP="$PWD"
SYSROOT="$TOP/deps/sysroot"
DL="$TOP/deps/dl"
SRC="$TOP/deps/src"
TOOLCHAIN="$TOP/cmake/toolchain-riscos.cmake"
MBEDTLS_VER=3.6.7
LIBSSH_VER=0.11.5
MBEDTLS_URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VER/mbedtls-$MBEDTLS_VER.tar.bz2"
LIBSSH_URL="https://www.libssh.org/files/${LIBSSH_VER%.*}/libssh-$LIBSSH_VER.tar.xz"
NPROC=$(nproc 2>/dev/null || echo 2)

mkdir -p "$DL" "$SRC" "$SYSROOT"
fetch() { [ -f "$DL/$2" ] || curl -fsSL -o "$DL/$2" "$1"; }
fetch "$MBEDTLS_URL" "mbedtls-$MBEDTLS_VER.tar.bz2"
fetch "$LIBSSH_URL"  "libssh-$LIBSSH_VER.tar.xz"

# --- mbedTLS -------------------------------------------------------------
if [ ! -f "$SYSROOT/lib/libmbedcrypto.a" ]; then
rm -rf "$SRC/mbedtls-$MBEDTLS_VER"
tar -xjf "$DL/mbedtls-$MBEDTLS_VER.tar.bz2" -C "$SRC"
cd "$SRC/mbedtls-$MBEDTLS_VER"
for p in "$TOP"/deps/patches/mbedtls-*.patch; do [ -f "$p" ] && patch -p1 < "$p"; done
cmake -S . -B build-riscos -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DRISCOS_SYSROOT="$SYSROOT" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$SYSROOT" \
    -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DMBEDTLS_FATAL_WARNINGS=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
    -DGEN_FILES=OFF \
    -DMBEDTLS_USER_CONFIG_FILE="$TOP/deps/mbedtls_riscos_config.h"
cmake --build build-riscos -j"$NPROC"
cmake --install build-riscos
fi

# --- libssh --------------------------------------------------------------
# No constructor/destructor attributes and no __thread: UnixLib has not set
# up thread-local storage when global constructors run, which segfaults in
# libssh_constructor().  The server calls ssh_init() itself.
rm -rf "$SRC/libssh-$LIBSSH_VER"
tar -xJf "$DL/libssh-$LIBSSH_VER.tar.xz" -C "$SRC"
cd "$SRC/libssh-$LIBSSH_VER"
for p in "$TOP"/deps/patches/libssh-*.patch; do [ -f "$p" ] && patch -p1 < "$p"; done
cmake -S . -B build-riscos -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DRISCOS_SYSROOT="$SYSROOT" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$SYSROOT" \
    -DBUILD_SHARED_LIBS=OFF -DWITH_MBEDTLS=ON -DMBEDTLS_ROOT_DIR="$SYSROOT" \
    -DWITH_SERVER=ON -DWITH_SFTP=OFF -DWITH_ZLIB=OFF -DWITH_GSSAPI=OFF \
    -DWITH_PCAP=OFF -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF -DWITH_SYMBOL_VERSIONING=OFF \
    -DWITH_GEX=ON -DWITH_NACL=OFF -DWITH_BLOWFISH_CIPHER=OFF \
    -DCMAKE_C_FLAGS="-D_GNU_SOURCE" \
    -DHAVE_CONSTRUCTOR_ATTRIBUTE=0 -DHAVE_DESTRUCTOR_ATTRIBUTE=0 \
    -DHAVE_GCC_THREAD_LOCAL_STORAGE=0
cmake --build build-riscos -j"$NPROC"
cmake --install build-riscos
echo "Dependencies installed in $SYSROOT"

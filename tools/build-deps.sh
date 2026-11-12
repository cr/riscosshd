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
MBEDTLS_SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
LIBSSH_SHA256=6898ba9dd836d618b71dc7a4bb786a502c173cef5cafbf20fe5e0567ba4ea30c
NPROC=$(nproc 2>/dev/null || echo 2)

mkdir -p "$DL" "$SRC" "$SYSROOT"
# fetch URL FILE SHA256: download if missing, then verify the digest.
fetch() {
    [ -f "$DL/$2" ] || curl -fsSL -o "$DL/$2" "$1"
    echo "$3  $DL/$2" | sha256sum -c - >/dev/null || {
        echo "checksum mismatch for $2; delete it and retry, or update the pin" >&2
        exit 1
    }
}
fetch "$MBEDTLS_URL" "mbedtls-$MBEDTLS_VER.tar.bz2" "$MBEDTLS_SHA256"
fetch "$LIBSSH_URL"  "libssh-$LIBSSH_VER.tar.xz" "$LIBSSH_SHA256"

# --- mbedTLS -------------------------------------------------------------
# Rebuild mbedTLS when its configuration or the toolchain file is newer
# than the library, not only when the library is missing.
if [ ! -f "$SYSROOT/lib/libmbedcrypto.a" ] \
   || [ "$TOP/deps/mbedtls_riscos_config.h" -nt "$SYSROOT/lib/libmbedcrypto.a" ] \
   || [ "$TOOLCHAIN" -nt "$SYSROOT/lib/libmbedcrypto.a" ]; then
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
    -DWITH_SERVER=ON -DWITH_SFTP=ON -DWITH_ZLIB=OFF -DWITH_GSSAPI=OFF \
    -DWITH_PCAP=OFF -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF -DWITH_SYMBOL_VERSIONING=OFF \
    -DWITH_GEX=ON -DWITH_NACL=OFF -DWITH_BLOWFISH_CIPHER=OFF \
    -DCMAKE_C_FLAGS="-D_GNU_SOURCE -I$TOP/deps/riscos-compat" \
    -DHAVE_CONSTRUCTOR_ATTRIBUTE=0 -DHAVE_DESTRUCTOR_ATTRIBUTE=0 \
    -DHAVE_GCC_THREAD_LOCAL_STORAGE=0
cmake --build build-riscos -j"$NPROC"
cmake --install build-riscos
echo "Dependencies installed in $SYSROOT"

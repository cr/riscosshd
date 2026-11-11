#!/bin/sh
# Runs inside the GCCSDK container (or any machine with GCCSDK + cmake).
# Builds the dependencies into deps/sysroot if missing, then the server.
set -e
cd "$(dirname "$0")/.."
SYSROOT="$PWD/deps/sysroot"
case "$1" in
    deps)  rm -rf "$SYSROOT" ;;
    clean) rm -rf build; exit 0 ;;
esac
if [ ! -f "$SYSROOT/lib/libssh.a" ]; then
    sh tools/build-deps.sh
fi
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscos.cmake \
      -DRISCOS_SYSROOT="$SYSROOT" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
cmake --build build --target package-zip

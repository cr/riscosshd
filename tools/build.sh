#!/bin/sh
# Build everything inside the GCCSDK container.
#   tools/build.sh            build deps (once) and the server
#   tools/build.sh deps       force a rebuild of mbedTLS and libssh
#   tools/build.sh clean      remove build output (keeps deps/sysroot)
set -e
cd "$(dirname "$0")/.."
. tools/docker-env.sh
IMAGE=riscosshd-gccsdk
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build --platform linux/amd64 -t "$IMAGE" docker
fi
exec docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work "$IMAGE" sh tools/ci-build.sh "$@"

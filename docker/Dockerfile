# GCCSDK cross-compiler for RISC OS plus CMake, used for building libssh,
# mbedTLS and this server.  The base image is linux/amd64 only; on Apple
# Silicon it runs under emulation (Colima with binfmt, or Docker Desktop).
# Pinned by digest (tag "latest" as of 2026-11-11, GCC 4.7.4 Release 5).
FROM --platform=linux/amd64 riscosdotinfo/riscos-gccsdk-4.7@sha256:c97c21725d627699ca7fde4e946c86ba5513b1af391ad8b6172fcbdd0106df41
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends cmake ca-certificates curl zip \
 && rm -rf /var/lib/apt/lists/*
# cmake/toolchain-riscos.cmake finds the compiler at /home/riscos/cross/bin.

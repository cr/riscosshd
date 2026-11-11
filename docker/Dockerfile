# GCCSDK cross-compiler for RISC OS plus CMake, used for building libssh,
# mbedTLS and this server.  The base image is linux/amd64 only; on Apple
# Silicon it runs under emulation (Colima with binfmt, or Docker Desktop).
FROM --platform=linux/amd64 riscosdotinfo/riscos-gccsdk-4.7:latest
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends cmake ca-certificates curl zip \
 && rm -rf /var/lib/apt/lists/*
ENV GCCSDK_INSTALL_CROSSBIN=/home/riscos/cross/bin \
    GCCSDK_INSTALL_ENV=/home/riscos/env \
    PATH=/home/riscos/cross/bin:/home/riscos/env:$PATH
WORKDIR /work

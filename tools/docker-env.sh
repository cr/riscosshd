#!/bin/sh
# SPDX-License-Identifier: MIT
# Source this before using the docker CLI.  With Colima it points the CLI
# at Colima's socket and at a config without the Docker Desktop credential
# helper; with Docker Desktop, Podman or a CI runner it changes nothing.
# Used by the Makefile and tools/container-engine.sh.
colima_sock="$HOME/.colima/default/docker.sock"
if [ -z "$DOCKER_HOST" ] && [ -S "$colima_sock" ]; then
    DOCKER_HOST="unix://$colima_sock"
    export DOCKER_HOST
    if [ -z "$DOCKER_CONFIG" ]; then
        DOCKER_CONFIG="${TMPDIR:-/tmp}/riscosshd-dockercfg"
        mkdir -p "$DOCKER_CONFIG"
        [ -f "$DOCKER_CONFIG/config.json" ] || echo '{}' > "$DOCKER_CONFIG/config.json"
        export DOCKER_CONFIG
    fi
fi

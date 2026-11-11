#!/bin/sh
# Source this to point the docker CLI at Colima without the Docker Desktop
# credential helper.  Used by tools/build.sh.
: "${DOCKER_HOST:=unix://$HOME/.colima/default/docker.sock}"
export DOCKER_HOST
if [ -z "$DOCKER_CONFIG" ] && [ -S "${DOCKER_HOST#unix://}" ]; then
    DOCKER_CONFIG="${TMPDIR:-/tmp}/riscosshd-dockercfg"
    mkdir -p "$DOCKER_CONFIG"
    [ -f "$DOCKER_CONFIG/config.json" ] || echo '{}' > "$DOCKER_CONFIG/config.json"
    export DOCKER_CONFIG
fi

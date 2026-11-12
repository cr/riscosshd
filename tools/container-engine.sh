#!/bin/sh
# Pick a container engine able to run the linux/amd64 GCCSDK image.
# Prints "docker" or "podman" on stdout and exits 0, or explains on
# stderr and exits 1.  The Makefile sources tools/docker-env.sh for the
# Colima socket and credential settings.
#
#   tools/container-engine.sh          detect
#   tools/container-engine.sh start    also start Colima if it is installed but stopped
. "$(dirname "$0")/docker-env.sh" 2>/dev/null

if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    echo docker
    exit 0
fi
if command -v podman >/dev/null 2>&1 && podman info >/dev/null 2>&1; then
    echo podman
    exit 0
fi
if command -v colima >/dev/null 2>&1; then
    if [ "$1" = start ]; then
        echo "Starting Colima..." >&2
        colima start >&2 || exit 1
        . "$(dirname "$0")/docker-env.sh"
        if docker info >/dev/null 2>&1; then
            echo docker
            exit 0
        fi
    else
        echo "Colima is installed but not running: run 'make engine' (or 'colima start')" >&2
        exit 1
    fi
fi
echo "No container engine found. Install Docker, Podman, or Colima (brew install colima docker)." >&2
exit 1

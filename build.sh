#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
#
# Wii U GX2 PoC build wrapper — devkitpro/devkitppc Docker image.
#
# Usage:
#   ./build.sh              # configure + build + package all PoCs
#   ./build.sh configure    # configure only
#   ./build.sh clean        # rm -rf build/
#   ./build.sh shell        # interactive shell inside the container

set -e

DOCKER="${DOCKER:-docker}"
IMAGE="${IMAGE:-devkitpro/devkitppc:latest}"
SRCDIR="$(pwd)"
BUILDDIR="${BUILDDIR:-build}"
ACTION="${1:-build}"

case "$ACTION" in
    clean)
        rm -rf "$BUILDDIR"
        echo "Cleaned $BUILDDIR/"
        exit 0
        ;;
    shell)
        export MSYS_NO_PATHCONV=1
        exec "$DOCKER" run --rm -it -v "$SRCDIR:/src" "$IMAGE" bash
        ;;
    configure|build)
        ;;
    *)
        echo "Unknown action: $ACTION (expected: configure | build | clean | shell)"
        exit 1
        ;;
esac

export MSYS_NO_PATHCONV=1

CONFIGURE_CMD='cd /src && cmake -B '"$BUILDDIR"' \
    -DCMAKE_TOOLCHAIN_FILE=/src/cmake/wiiu-devkitpro.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -G "Unix Makefiles"'

BUILD_CMD='cd /src/'"$BUILDDIR"' && make -j8'

if [ "$ACTION" = "configure" ]; then
    SCRIPT="$CONFIGURE_CMD"
else
    SCRIPT="$CONFIGURE_CMD && $BUILD_CMD"
fi

exec "$DOCKER" run --rm -v "$SRCDIR:/src" "$IMAGE" bash -c "$SCRIPT"

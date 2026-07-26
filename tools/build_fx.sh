#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
#
# Build the PC audition tool. Prefers a local compiler; falls back to the one
# inside the devkitPro container, which this project already requires — so the
# tool costs no new dependency on a machine that can build the PoCs at all.
#
# Note that the container's gcc targets Linux x86-64, i.e. little-endian, while
# the same source compiled for the console is big-endian PowerPC. That is the
# point rather than a caveat: echo.h does arithmetic on native integers, and a
# tool that only agreed with the console on one byte order would be hiding
# something.

set -e
cd "$(dirname "$0")"

SRC=fx_render.c
OUT=${OUT:-fx_render}

for cc in gcc cc clang /c/msys64/mingw64/bin/gcc /c/msys64/ucrt64/bin/gcc; do
    if command -v "$cc" >/dev/null 2>&1; then
        echo "[fx] $cc -> $OUT"
        "$cc" -O2 -Wall -Wextra -o "$OUT" "$SRC" -lm
        exit 0
    fi
done

echo "[fx] no local compiler; using the devkitPro container's"
export MSYS_NO_PATHCONV=1
exec "${DOCKER:-docker}" run --rm -v "$(cd .. && pwd):/src" \
    "${IMAGE:-devkitpro/devkitppc:latest}" \
    bash -c "cd /src/tools && gcc -O2 -Wall -Wextra -o $OUT $SRC -lm"

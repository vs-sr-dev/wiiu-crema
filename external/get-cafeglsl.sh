#!/usr/bin/env bash
# Fetch the CafeGLSL runtime shader compiler (not redistributed here).
# https://github.com/Exzap/CafeGLSL — by Exzap & Crementif, Mesa-based.
#
# Install the result to:
#   Cemu:          <Cemu data dir>/cafeLibs/glslcompiler.rpl
#   real hardware: sd:/wiiu/libs/glslcompiler.rpl
set -e
cd "$(dirname "$0")"
URL="https://github.com/Exzap/CafeGLSL/releases/download/v0.2.0/glslcompiler.rpl"
curl -L --retry 3 -o glslcompiler.rpl "$URL"
echo "OK: $(ls -la glslcompiler.rpl)"

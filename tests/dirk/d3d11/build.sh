#!/usr/bin/env bash
# DXVK's own D3D11 test programs (removed upstream in 632e26f6), as Windows
# x64 programs for dirk on W64: build.sh OUT [NAME...] (default all). Needs
# MinGW-w64's x86_64-w64-mingw32-g++.
#
# triangle stops after TEST_FRAMES frames (default 120) and prints
# "triangle: N frames"; the others run once and print their results.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build.sh OUT [NAME...]}
shift
mkdir -p "$out"
names=${*:-compute deferred formats map_read streamout triangle}
for n in $names; do
  x86_64-w64-mingw32-g++ -std=c++17 -O1 -DNOMINMAX "$here/test_d3d11_$n.cpp" \
    -o "$out/d3d11-$n.exe" -ld3d11 -ldxgi -ld3dcompiler_47 -luuid -static
done

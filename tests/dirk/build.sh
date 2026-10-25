#!/usr/bin/env bash
# Builds dirk's host cube: the dirk objects (DXVK's front end and the dirk
# backend) and the test, linked with a packaged wink.
#
#   MESA=<wink Mesa tree> [VKD3D_COMPILER=vkd3d-compiler] [EMXX=em++] [WIRE=1] \
#     build.sh <wink-packaged-dir> <objects-dir> <out-dir>
#
# WIRE=1: the cube's calls go through the wire, whose guest side is
# src/dirk/wire.
#
# The objects are dirk's (objects.mk). cube.hlsl is compiled to DXBC with
# vkd3d-compiler.
set -euo pipefail
pkg=$1 objs=$2 out=$3
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
D=$(CDPATH='' cd -- "$HERE/../.." && pwd -P)
MESA=${MESA:?set MESA to the wink Mesa tree}
EMXX=${EMXX:-em++}
mkdir -p "$out"
dxbc=$out/dxbc
mkdir -p "$dxbc"
for s in vs ps; do
  ${VKD3D_COMPILER:-vkd3d-compiler} -x hlsl -b dxbc-tpf -p ${s}_4_0 -e ${s}_main \
    -o "$dxbc/$s.dxbc" "$HERE/cube.hlsl"
  (cd "$dxbc" && xxd -i -n cube_$s $s.dxbc > cube_$s.h)
done

wire=()
if [ -n "${WIRE:-}" ]; then
  EMCC=${EMCC:-${EMXX%++}cc}
  for f in "$D/src/dirk/wire/dirk_guest.c" "$D/src/dirk/wire/gen/dirk_wire_guest.c"; do
    "$EMCC" -std=gnu11 -O2 -pthread -I"$D/include/native" -I"$D/include/native/windows" \
      -I"$D/include/native/directx" -I"$D/src/dirk/wire" -I"$D/src/dirk/wire/gen" \
      -c "$f" -o "$out/$(basename "$f" .c).o"
    wire+=("$out/$(basename "$f" .c).o")
  done
fi

flags=()
while IFS= read -r line; do flags+=("$line"); done < "$pkg/wink.rsp"

"$EMXX" -std=c++17 -O2 -pthread -fwasm-exceptions -DNOMINMAX \
  -I"$D/include/native" -I"$D/include/native/windows" -I"$D/include/native/directx" \
  -I"$pkg/include" -I"$MESA/src/gallium/drivers/gex_wgpu" -I"$dxbc" \
  ${WIRE:+-DDIRK_WIRE_TEST} -c "$HERE/d3d11cube.cpp" -o "$out/d3d11cube.o"

"$EMXX" -O2 "$out/d3d11cube.o" "$objs"/*.o ${wire[@]+"${wire[@]}"} \
  "$pkg/lib/libwink.a" "$pkg/lib/libwink_webgpu.a" "$pkg/lib/libwink_tint.a" \
  "${flags[@]}" \
  --js-library="$HERE/library_dirk_test.js" \
  -sPTHREAD_POOL_SIZE=4 \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createDirkCube \
  -sENVIRONMENT=web,worker --no-entry \
  -sEXPORTED_FUNCTIONS=_dirk_cube_main,_malloc,_free -sEXPORTED_RUNTIME_METHODS=HEAPU8 \
  -sINITIAL_MEMORY=128MB -sASSERTIONS=1 --profiling-funcs \
  -o "$out/d3d11cube.js"
cp "$HERE/index.html" "$out/index.html"
echo "$out/index.html"

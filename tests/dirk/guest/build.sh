#!/usr/bin/env bash
# Builds dirk's guest test: tri, a static x86-64 Linux program whose D3D11
# calls go through the wire to gex's /dev/gex-d3d (and tri.exe, below).
#
#   [CC=x86_64-linux-musl-gcc] [VKD3D_COMPILER=vkd3d-compiler] build.sh <out-dir>
#
# Without VKD3D_COMPILER the shaders are the prebuilt ones in gen/; with it
# they are compiled from tri.hlsl (and gen/ can be refreshed from <out-dir>).
set -euo pipefail
out=$1
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
D=$(CDPATH='' cd -- "$HERE/../../.." && pwd -P)
CC=${CC:-$HOME/toolchains/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc}
mkdir -p "$out"
SHADERS=$HERE/gen
if [ -n "${VKD3D_COMPILER:-}" ]; then
SHADERS=$out
for s in vs ps; do
  $VKD3D_COMPILER -x hlsl -b dxbc-tpf -p ${s}_4_0 -e ${s}_main \
    -o "$out/$s.dxbc" "$HERE/tri.hlsl"
  (cd "$out" && xxd -i -n tri_$s $s.dxbc > tri_$s.h)
done
fi
"$CC" -std=gnu11 -O2 -static -pthread \
  -I"$D/include/native" -I"$D/include/native/windows" -I"$D/include/native/directx" \
  -I"$D/src/dirk/wire" -I"$D/src/dirk/wire/gen" -I"$HERE" -I"$SHADERS" \
  "$HERE/tri.c" "$HERE/gex_transport.c" "$D/src/dirk/wire/dirk_guest.c" "$D/src/dirk/wire/dirk_draw.c" \
  "$D/src/dirk/wire/gen/dirk_wire_guest.c" -o "$out/tri"
echo "$out/tri"
# And tri.exe, the same test as a Windows x64 program (W64, the shim's
# d3d11.dll), when MinGW is there.
MINGW=${MINGW:-x86_64-w64-mingw32-gcc}
if command -v "$MINGW" >/dev/null; then
  "$MINGW" -std=gnu11 -O2 -I"$SHADERS" "$HERE/tri.c" -o "$out/tri.exe" -ld3d11 -ldxguid -luuid
  echo "$out/tri.exe"
fi

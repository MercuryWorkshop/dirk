#!/usr/bin/env bash
# Builds dirk's Windows shim for W64 (gex's 64-bit native Wine): d3d11.dll
# (the wire's guest side, src/dirk/pe and src/dirk/wire) and dxgi.dll (its
# entry points forwarded to d3d11.dll), native wasm PE code, as relocatable
# objects and export lists gex's wine/stage-native.sh links
# into the native PE core (WINE64_EXTRA_DLLS=OUT). Their Unix side,
# src/dirk/pe/unixlib.c, is linked with the loader (wine/link-unix64.sh).
#
#   GEX_WASM64_SYSROOT=... build.sh OUT WINE_BUILD WINE_SRC GEX
#
# WINE_BUILD is a W64 wasm64 build tree (its winegcc, winebuild, import
# libraries and generated headers), WINE_SRC its source, GEX gex's tree (wine/wcc64).
set -euo pipefail
[ $# -eq 4 ] || { sed -n 2,12p "$0" >&2; exit 2; }
mkdir -p "$1"
out=$(cd "$1" && pwd) B=$(cd "$2" && pwd) W=$(cd "$3" && pwd) G=$(cd "$4" && pwd)
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
D=$(CDPATH='' cd -- "$HERE/../../../src/dirk" && pwd -P)
: "${GEX_WASM64_SYSROOT:?set GEX_WASM64_SYSROOT}"
WCC=$G/wine/wcc64
# A tree of the memory64 flavour (configured with wcc64 -mgex-memory64): the
# shim is compiled for it too.
grep -q '^CC *=.*-mgex-memory64' "$B/Makefile" && WCC="$WCC -mgex-memory64"
tools=$(grep -m1 -o '[^ ]*/tools/winebuild/winebuild' "$B/Makefile")
tools=${tools%/winebuild/winebuild}
CFLAGS=(-O2 -fPIC -fvisibility=hidden -fno-strict-aliasing -D_WIN32 -D_UCRT -fno-builtin -fshort-wchar
        -Wno-format -I"$D/wire" -I"$D/wire/gen" -I"$D/pe" -I"$B/include" -I"$W/include" -I"$W/include/msvcrt")
tmp=$(mktemp -d "${TMPDIR:-/tmp}/dirk-w64.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
objs=()
for c in "$D/pe/d3d11_main.c" "$D/pe/guid.c" "$D/wire/dirk_guest.c" "$D/wire/dirk_draw.c" "$D/wire/gen/dirk_wire_guest.c"; do
    o=$tmp/$(basename "$c" .c).o
    $WCC "${CFLAGS[@]}" -c -o "$o" "$c" &
    objs+=("$o")
done
wait
link() { # DLL SPEC OBJS...
    local dll=$1 spec=$2
    shift 2
    (cd "$tmp" && TMPDIR=$tmp "$tools/winegcc/winegcc" -o "$out/$dll.so" --wine-objdir "$B" \
        --winebuild "$tools/winebuild/winebuild" --cc-cmd="$WCC" -b wasm64-unknown-linux-musl \
        -mno-cygwin -fPIC -shared "$spec" -Wb,-F,"$dll" "$@" "$B/libs/winecrt0/libwinecrt0.a" \
        -lucrtbase -lkernel32 -lntdll)
    cp "$spec" "$out/${dll%.dll}.spec"
}
link d3d11.dll "$D/pe/d3d11.spec" "${objs[@]}"
echo "ucrtbase.dll,kernel32.dll,ntdll.dll" > "$out/d3d11.imports"
link dxgi.dll "$D/pe/dxgi.spec"
echo "d3d11.dll,kernel32.dll,ntdll.dll" > "$out/dxgi.imports"
rm -f "$out"/*.so "$out"/*.so.64.wasm
echo "$out: d3d11.dll dxgi.dll"

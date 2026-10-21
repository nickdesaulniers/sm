#!/usr/bin/env bash
# Builds the Game Boy Advance cartridge, sm.gba, from your own Super Metroid
# ROM:
#
#     ./build-gba.sh [ROM]
#
# ROM defaults to sm.smc here (the US release, headerless, sha1
# da957f0d63d14cb441d215462904c4fa8519c613). Needs clang, lld, llvm-objcopy,
# CMake, Python 3 and git (the first build fetches LLVM's compiler-rt
# sources, a few MB). The cartridge comes out as sm.gba, here.
set -euo pipefail
cd "$(dirname "$0")"
rom=${1:-sm.smc}
want=da957f0d63d14cb441d215462904c4fa8519c613

if [ ! -f "$rom" ]; then
    echo "error: no ROM at $rom: put your Super Metroid ROM there, as sm.smc" >&2
    exit 1
fi
for tool in clang ld.lld cmake python3 git; do
    if ! command -v "$tool" >/dev/null; then
        echo "error: $tool not found (see README.md)" >&2
        exit 1
    fi
done
have=$(python3 -c 'import hashlib, sys; print(hashlib.sha1(open(sys.argv[1], "rb").read()).hexdigest())' "$rom")
if [ "$have" != "$want" ]; then
    echo "error: $rom isn't the ROM this was made for (sha1 $have, not $want)" >&2
    echo "(a 512-byte copier header? the US release is needed, without one)" >&2
    exit 1
fi

cmake -B build-gba -DCMAKE_TOOLCHAIN_FILE=cmake/gba-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release -DSM_ROM="$(realpath "$rom")"
cmake --build build-gba -j"$(nproc 2>/dev/null || echo 4)"
cp build-gba/sm.gba sm.gba
echo "made sm.gba: run it in mGBA, or on a flash cartridge"

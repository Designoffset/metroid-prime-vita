#!/bin/bash
# Fetches the upstream sources at the commits this port was built against and applies the Vita patches.
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; cd "$ROOT"
clone() { [ -d "$2" ] || git clone -q "$1" "$2"; git -C "$2" checkout -q "$3"; }
clone https://github.com/Odrannnn/MetroidPrimePort.git mpp fad2b448062729a9452e7d5880a1e0d7cb87ebfd
git -C mpp apply "$ROOT/patches/MetroidPrimePort.patch"
mkdir -p vita/libsrc
L=vita/libsrc
clone https://github.com/vitasdk/buildscripts.git vita/buildscripts 207c66b
clone https://github.com/libsdl-org/SDL.git $L/SDL3 8e37db5
clone https://github.com/Rinnegatamante/SceShaccCgExt.git $L/SceShaccCgExt 1021923
clone https://github.com/abseil/abseil-cpp.git $L/abseil 4447c75 && git -C $L/abseil apply "$ROOT/patches/abseil.patch"
clone https://github.com/encounter/dawn.git $L/dawn 1155e0e
clone https://github.com/fmtlib/fmt.git $L/fmt 407c905
clone https://github.com/freetype/freetype.git $L/freetype 0a0221a
clone https://github.com/ocornut/imgui.git $L/imgui 4806a19
clone https://github.com/pnggroup/libpng.git $L/libpng 3061454
clone https://github.com/Rinnegatamante/math-neon.git $L/math-neon 0faab81
clone https://github.com/yifanlu/taiHEN.git $L/taihen 309b380
clone https://github.com/wolfpld/tracy.git $L/tracy 30997d5 && git -C $L/tracy apply "$ROOT/patches/tracy.patch"
clone https://github.com/Rinnegatamante/vitaGL.git $L/vitaGL a85cb8d
clone https://github.com/Rinnegatamante/vitaShaRK.git $L/vitaShaRK df24065
clone https://github.com/Cyan4973/xxHash.git $L/xxhash e626a72
clone https://github.com/madler/zlib.git $L/zlib 51b7f2a && git -C $L/zlib apply "$ROOT/patches/zlib.patch"
clone https://github.com/facebook/zstd.git $L/zstd f8745da
echo "Sources ready. Next: vita/fetch.sh, vita/build_tc.sh, vita/build_deps.sh, vita/build_libs.sh, vita/build_game.sh"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
V=$ROOT/vita
M=$ROOT/mpvita
export VITASDK=$V/sdk PATH=$V/sdk/bin:$V/deps/bin:$PATH
T=arm-vita-eabi; PFX=$VITASDK/$T; TC=$VITASDK/share/vita.toolchain.cmake; B=$V/b/libs; mkdir -p $B
step(){ echo "=== $(date +%H:%M:%S) $*"; }
fail(){ echo "FAIL $*"; exit 1; }
while ! grep -q TOOLCHAIN_DONE $V/tc.log; do grep -q "FAIL" $V/tc.log && { echo TOOLCHAIN_FAILED; exit 1; }; sleep 30; done
step sanity
printf '#include <vector>\n#include <thread>\n#include <cstdio>\nint main(){std::vector<int> v{1,2}; std::thread t([]{}); t.join(); printf("%%zu\\n", v.size());}\n' > $B/t.cpp
$T-g++ -std=c++20 -O2 $B/t.cpp -o $B/t.elf -lpthread >$B/sanity.log 2>&1 || fail sanity
step zlib
cmake -S $V/libsrc/zlib -B $B/zlib -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_INSTALL_PREFIX=$PFX -DCMAKE_BUILD_TYPE=Release -DZLIB_BUILD_EXAMPLES=OFF >$B/zlib.log 2>&1 || fail zlib-cfg
cmake --build $B/zlib -j2 >>$B/zlib.log 2>&1 && cmake --install $B/zlib >>$B/zlib.log 2>&1 || fail zlib
rm -f $PFX/lib/libz.so* 2>/dev/null
step SDL3
cmake -S $V/libsrc/SDL3 -B $B/sdl3 -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_INSTALL_PREFIX=$PFX -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF >$B/sdl3.log 2>&1 || fail sdl3-cfg
cmake --build $B/sdl3 -j2 >>$B/sdl3.log 2>&1 && cmake --install $B/sdl3 >>$B/sdl3.log 2>&1 || fail sdl3
step webgpu-null+mininod
G=$V/libsrc/dawn
mkdir -p $PFX/include/dawn $PFX/include/webgpu $B/wn
cp -r $G/gen/include/dawn/. $PFX/include/dawn/ && cp -r $G/gen/include/webgpu/. $PFX/include/webgpu/
for f in webgpu_null_core webgpu_null_stubs; do $T-g++ -std=c++20 -O2 -w -I$PFX/include -I$M/webgpu_null -c $M/webgpu_null/$f.cpp -o $B/wn/$f.o || fail $f; done
$T-ar rcs $PFX/lib/libwebgpu_null.a $B/wn/*.o
$T-gcc -std=c11 -O2 -Wall -I$M/mininod -I$M/compat -c $M/mininod/mininod.c -o $B/wn/mininod.o || fail mininod
$T-ar rcs $PFX/lib/libmininod.a $B/wn/mininod.o && mkdir -p $PFX/include/nod && cp $M/mininod/nod.h $PFX/include/nod.h
mkdir -p $PFX/lib/cmake/Dawn $PFX/lib/cmake/nod
cat > $PFX/lib/cmake/Dawn/DawnConfig.cmake <<'CFG'
if(NOT TARGET dawn::webgpu_dawn)
  add_library(dawn::webgpu_dawn STATIC IMPORTED GLOBAL)
  get_filename_component(_p "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
  set_target_properties(dawn::webgpu_dawn PROPERTIES IMPORTED_LOCATION "${_p}/lib/libwebgpu_vita_empty.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_p}/include")
endif()
set(Dawn_FOUND TRUE)
CFG
cat > $PFX/lib/cmake/nod/nodConfig.cmake <<'CFG'
if(NOT TARGET nod::nod)
  add_library(nod::nod STATIC IMPORTED GLOBAL)
  get_filename_component(_p "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
  set_target_properties(nod::nod PROPERTIES IMPORTED_LOCATION "${_p}/lib/libmininod.a" INTERFACE_INCLUDE_DIRECTORIES "${_p}/include")
endif()
set(nod_FOUND TRUE)
CFG
step LIBS_DONE

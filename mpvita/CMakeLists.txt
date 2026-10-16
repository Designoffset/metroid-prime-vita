# PS Vita build wrapper for the Metroid Prime native port.
# Configure with the vitasdk toolchain:
#   cmake -S mpvita -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DMPP_DIR=... -DLIBSRC=...
cmake_minimum_required(VERSION 3.25)
project(metroid_prime_vita C CXX)

set(MPP_DIR "" CACHE PATH "MetroidPrimePort source tree")
set(LIBSRC "" CACHE PATH "Directory holding pre-fetched dependency sources")

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# ---- dependency providers: everything local, nothing downloaded ----
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)
foreach(pair "ABSEIL-CPP;abseil" "XXHASH;xxhash" "FMT;fmt" "PNG;libpng" "FREETYPE;freetype" "IMGUI;imgui"
             "SQLITE3;sqlite" "ZSTD;zstd" "TRACY;tracy")
  list(GET pair 0 _n)
  list(GET pair 1 _d)
  set(FETCHCONTENT_SOURCE_DIR_${_n} "${LIBSRC}/${_d}" CACHE PATH "" FORCE)
endforeach()
set(AURORA_DAWN_PROVIDER system CACHE STRING "" FORCE)
set(AURORA_NOD_PROVIDER system CACHE STRING "" FORCE)
set(AURORA_SDL3_PROVIDER system CACHE STRING "" FORCE)
set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(MP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MP_USE_CCACHE OFF CACHE BOOL "" FORCE)
set(MP_USE_MOLD OFF CACHE BOOL "" FORCE)
set(MP_EMBED_RESOURCES OFF CACHE BOOL "" FORCE)
set(TRACY_ENABLE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Small POSIX compatibility headers the Vita's newlib lacks (sys/ioctl.h, ...).
add_compile_options(-isystem "${CMAKE_CURRENT_SOURCE_DIR}/compat")
# Match the PC/GameCube data layout: 4-byte enums (ARM EABI defaults to short enums).
add_compile_options(-fno-short-enums)
add_link_options(-Wl,--no-enum-size-warning)

# Vita executables must not be position-independent (vita-elf-create rejects GOT
# relocations); make CMake's PIC/PIE settings no-ops.
foreach(_lang C CXX)
  set(CMAKE_${_lang}_COMPILE_OPTIONS_PIC "")
  set(CMAKE_${_lang}_COMPILE_OPTIONS_PIE "")
endforeach()
set(CMAKE_POSITION_INDEPENDENT_CODE OFF CACHE BOOL "" FORCE)

# GNU ld supports archive groups; CMake only auto-detects this on Linux.
foreach(_lang C CXX)
  set(CMAKE_${_lang}_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
  set(CMAKE_${_lang}_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
endforeach()

add_subdirectory("${MPP_DIR}" mpp)

# sqlite (Aurora's pipeline cache): no mmap/WAL/dlopen on the Vita. Aurora's
# "PRAGMA journal_mode=WAL" then simply keeps the default rollback journal.
target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_WAL=1 SQLITE_MAX_MMAP_SIZE=0 SQLITE_OMIT_LOAD_EXTENSION=1)

# ---- Vita executable ----
set(_dbg "${CMAKE_CURRENT_SOURCE_DIR}/vita")
# Desktop-only features replaced by Vita versions.
get_target_property(_srcs metroid_prime_port SOURCES)
list(REMOVE_ITEM _srcs platform/port_crash.cpp platform/port_discord.cpp)
set_property(TARGET metroid_prime_port PROPERTY SOURCES ${_srcs})
target_sources(metroid_prime_port PRIVATE
  ${_dbg}/vita_main.cpp
  ${_dbg}/early_boot.c
  ${_dbg}/vita_prof.cpp
  ${_dbg}/vita_thp_async.cpp
  ${_dbg}/port_crash_vita.cpp
  ${_dbg}/port_discord_vita.cpp
  ${_dbg}/debugScreen.c
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/posix_compat.c
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/printf_c99.c)
target_include_directories(metroid_prime_port PRIVATE ${_dbg})

# ---- webgpu_vita: the WebGPU API on vitaGL (replaces Dawn) ----
set(_wv "${CMAKE_CURRENT_SOURCE_DIR}/webgpu_vita")
set(_wv_sources ${_wv}/wv_core.cpp ${_wv}/wv_exec.cpp ${_wv}/wv_special.cpp ${_wv}/wv_gx.cpp ${_wv}/wv_cg.cpp)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/wv_stubs.cpp
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/wv_stubs.stamp
  COMMAND Python3::Interpreter ${_wv}/gen_stubs.py "$ENV{VITASDK}/arm-vita-eabi/include/dawn/webgpu.h"
          ${CMAKE_CURRENT_BINARY_DIR}/wv_stubs.cpp ${_wv_sources}
  DEPENDS ${_wv}/gen_stubs.py ${_wv_sources}
  COMMENT "webgpu_vita: generating stubs")
# The stub file is generated in this directory; the game target lives in a subdirectory, so a
# custom target carries the dependency across.
add_custom_target(wv_stubs DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/wv_stubs.cpp)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/wv_stubs.cpp PROPERTIES GENERATED TRUE)
target_sources(metroid_prime_port PRIVATE ${_wv_sources} ${CMAKE_CURRENT_BINARY_DIR}/wv_stubs.cpp)
add_dependencies(metroid_prime_port wv_stubs)
target_link_libraries(metroid_prime_port PRIVATE absl::flat_hash_map absl::flat_hash_set)
target_include_directories(metroid_prime_port PRIVATE ${_wv} "${MPP_DIR}/extern/aurora/lib")
# Force pthread_cancel in (the driver's -pthread would add libpthread twice). Without it
# pthread_cancel stays an unresolved weak symbol, libstdc++ concludes threads are
# inactive, and the first std::call_once throws system_error(-1) ("Unknown error").
# The rwlock functions are weak references from std::shared_mutex: unresolved, a
# shared lock would jump to address 0.
# printf_c99.c: C99 printf sizes (%zu and friends) on top of the Vita's newlib.
target_link_options(metroid_prime_port PRIVATE -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/vita/segment_gap.ld)
target_link_options(metroid_prime_port PRIVATE -Wl,--wrap=_vfprintf_r -Wl,--wrap=_svfprintf_r
  -Wl,--wrap=_vfiprintf_r -Wl,--wrap=_svfiprintf_r)
target_link_options(metroid_prime_port PRIVATE -Wl,-u,pthread_cancel
  -Wl,-u,pthread_rwlock_rdlock -Wl,-u,pthread_rwlock_wrlock -Wl,-u,pthread_rwlock_unlock
  -Wl,-u,pthread_rwlock_tryrdlock -Wl,-u,pthread_rwlock_trywrlock)
target_link_libraries(metroid_prime_port PRIVATE
  vitaGL vitashark SceShaccCgExt taihen_stub SceShaccCg_stub mathneon
  SceGxm_stub SceCommonDialog_stub SceKernelDmacMgr_stub SceIme_stub SceAppUtil_stub
  SceCtrl_stub SceDisplay_stub ScePower_stub SceAppMgr_stub SceSysmodule_stub SceRtc_stub SceJpeg_stub)

set(VITA_APP_NAME "Metroid Prime")
set(VITA_TITLEID "MPRM00001")
set(VITA_VERSION "01.49")
# ATTRIBUTE2=12: extended memory budget for the app.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

vita_create_self(eboot.bin metroid_prime_port UNSAFE)
vita_create_vpk(MetroidPrime.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
       ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
       ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
       ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)

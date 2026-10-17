# PC test harness (headless). docs/RULES.md section 5:
#   cmake -B build-pc -S tools/harness && cmake --build build-pc -j
#   ./build-pc/harness --rom path/to/nbbatman.zip --frames 3600
cmake_minimum_required(VERSION 3.16)
project(rei_harness C CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(REI_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
include(${REI_ROOT}/src/core/core.cmake)

find_package(ZLIB REQUIRED)
target_link_libraries(rei_core PUBLIC ZLIB::ZLIB)

add_executable(harness main.c gem_check.c gm72_check.c ${REI_ROOT}/psp/src/ge_m107.c ${REI_ROOT}/psp/src/ge_m72.c)
target_include_directories(harness PRIVATE ${REI_ROOT}/psp/src)
target_compile_options(harness PRIVATE -Wall -Wextra)
find_package(Threads REQUIRED)
target_link_libraries(harness PRIVATE rei_core Threads::Threads)
# PSP YM2151-on-ME path (test with --ym-me, burn_ym2151.cpp REI_YM_ME)
target_compile_definitions(rei_core PUBLIC REI_YM_ME)

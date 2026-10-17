# PC software model of the PSP GE layer renderer (G2). Draws psp/src/ge_layers.c's draw lists in software per GE rules
# and compares pixel by pixel with the core's CPU-rendered screen (logic check without hardware).
#   cmake -B build-gemodel -S psp/tools/ge_model && cmake --build build-gemodel -j
#   ./build-gemodel/ge_model --rom nbbatman.zip --frames 3600 [--autoplay]
cmake_minimum_required(VERSION 3.16)
project(rei_ge_model C CXX)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
set(REI_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
include(${REI_ROOT}/src/core/core.cmake)
find_package(ZLIB REQUIRED)
target_link_libraries(rei_core PUBLIC ZLIB::ZLIB)
add_executable(ge_model main.c ${REI_ROOT}/psp/src/ge_layers.c)
target_include_directories(ge_model PRIVATE ${REI_ROOT}/psp/src)
target_compile_options(ge_model PRIVATE -Wall -Wextra)
target_link_libraries(ge_model PRIVATE rei_core)

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK environment variable is not set. Install VitaSDK and set VITASDK.")
  endif()
endif()

project(reirem C CXX)
# Vita build. Uses the shared core (src/core) at the repo root. Build: tools/build.sh vita (-> build-vita/)
set(REI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "ReiREM")
set(VITA_TITLEID  "M92A00001") # (NBBM00001 in the nbbatman-only days)
set(VITA_VERSION  "00.01")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Release flags (docs/RULES.md ch. 5)
set(REI_ARCH_FLAGS "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${REI_ARCH_FLAGS} -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${REI_ARCH_FLAGS} -Wall")
set(CMAKE_C_FLAGS_RELEASE   "-O3 -ffast-math -flto")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -ffast-math -flto")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-O3 -flto")
set(CMAKE_C_FLAGS_DEBUG   "-O0 -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

# UDP logger: OFF by default in release builds (compiled out)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(REI_LOG_DEFAULT OFF)
else()
  set(REI_LOG_DEFAULT ON)
endif()
option(REI_LOG "Include UDP logger" ${REI_LOG_DEFAULT})
set(REI_LOG_HOST "" CACHE STRING "PC IP to receive logs (empty = broadcast)")
set(REI_ARM_MHZ 444 CACHE STRING "ARM clock requested at startup (target 444)")
set(REI_INTERLEAVE_MUL 0 CACHE STRING "CPU slices per scanline (0 = FBNeo default 8, experimental)")
option(REI_PIPELINED "ADR-003/006 pipelined mode (main/sound event exchange)" ON)
option(REI_THREADED "Pipelined mode: run sound on a CPU1 thread" ON)
option(REI_RENDER_THREAD "Pipelined mode: run drawing on a CPU2 thread (ADR-007)" ON)
option(REI_AUTOPLAY "Deterministic auto input for measurement (same as harness --autoplay)" OFF)
option(REI_AUDIO_DUMP "Diag: save first 60 s of audio to ux0:data/reirem/dump_*.raw" OFF)

# Emulator core (minimal FBNeo sources + shim). Same definitions as the PC harness
include(${REI_ROOT}/src/core/core.cmake)

add_executable(${PROJECT_NAME}
  src/frontend/main.c
  src/frontend/video.c
  src/frontend/audio.c
  src/frontend/resample.c
  src/frontend/input.c
  src/frontend/threads.c
  src/frontend/settings.c
  src/frontend/menu.c
  src/frontend/states.c
  src/platform/clock.c
  src/platform/log.c
  src/platform/perf.c
  src/platform/hud.c
)

target_include_directories(${PROJECT_NAME} PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
# Version (git commit hash, options About): regenerate rei_version.h every build (tools/git_version.cmake)
add_custom_target(rei_version
  COMMAND ${CMAKE_COMMAND} -DSRC=${REI_ROOT} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/rei_version.h -P ${REI_ROOT}/tools/git_version.cmake
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/rei_version.h
  VERBATIM)
add_dependencies(${PROJECT_NAME} rei_version)
target_compile_definitions(${PROJECT_NAME} PRIVATE REI_ARM_MHZ=${REI_ARM_MHZ} REI_INTERLEAVE_MUL=${REI_INTERLEAVE_MUL})
if(REI_AUTOPLAY)
  target_compile_definitions(${PROJECT_NAME} PRIVATE REI_AUTOPLAY=1)
endif()
if(REI_AUDIO_DUMP)
  target_compile_definitions(${PROJECT_NAME} PRIVATE REI_AUDIO_DUMP=1)
endif()
if(REI_PIPELINED)
  target_compile_definitions(${PROJECT_NAME} PRIVATE REI_PIPELINED=1)
  if(REI_THREADED)
    target_compile_definitions(${PROJECT_NAME} PRIVATE REI_THREADED=1)
  endif()
  if(REI_RENDER_THREAD)
    target_compile_definitions(${PROJECT_NAME} PRIVATE REI_RENDER_THREAD=1)
  endif()
endif()

if(REI_LOG)
  target_compile_definitions(${PROJECT_NAME} PRIVATE REI_ENABLE_LOG)
  if(REI_LOG_HOST)
    target_compile_definitions(${PROJECT_NAME} PRIVATE REI_LOG_HOST="${REI_LOG_HOST}")
  endif()
endif()

target_link_libraries(${PROJECT_NAME}
  rei_core
  vita2d
  SceAudio_stub
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  ScePgf_stub
  ScePvf_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  SceAppUtil_stub
  ScePower_stub
  SceNet_stub
  SceNetCtl_stub
  png
  jpeg
  z
  m
  c
)

# Archive the unstripped .elf as artifacts/<commit-hash>.elf every build (for core dump analysis)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DELF=$<TARGET_FILE:${PROJECT_NAME}> -DSRC=${REI_ROOT}
          -P ${REI_ROOT}/tools/archive_elf.cmake
  VERBATIM)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
# VPK: vita_create_vpk does not quote FILE paths, so it fails when the path has spaces
# ("Vita Port"). Define the same steps directly with VERBATIM.
set(REI_SCE_SYS_FILES
  sce_sys/icon0.png
  sce_sys/livearea/contents/bg.png
  sce_sys/livearea/contents/startup.png
  sce_sys/livearea/contents/template.xml
)
# Bezel artwork is not packed in the VPK (copyright): users put it in ux0:data/reirem/artwork/<set>.png
set(REI_VPK_ADD_ARGS "")
set(REI_VPK_DEPS "")
foreach(f ${REI_SCE_SYS_FILES})
  list(APPEND REI_VPK_ADD_ARGS -a "${CMAKE_SOURCE_DIR}/${f}=${f}")
  list(APPEND REI_VPK_DEPS "${CMAKE_SOURCE_DIR}/${f}")
endforeach()

set(REI_SFO "${CMAKE_BINARY_DIR}/param.sfo")
set(REI_VPK "${CMAKE_BINARY_DIR}/reirem.vpk")
add_custom_command(OUTPUT "${REI_SFO}"
  COMMAND vita-mksfoex -s APP_VER=${VITA_VERSION} -s TITLE_ID=${VITA_TITLEID}
          "${VITA_APP_NAME}" "${REI_SFO}"
  VERBATIM)
add_custom_command(OUTPUT "${REI_VPK}"
  COMMAND vita-pack-vpk -s "${REI_SFO}" -b "${CMAKE_BINARY_DIR}/eboot.bin"
          ${REI_VPK_ADD_ARGS} "${REI_VPK}"
  DEPENDS "${REI_SFO}" "${CMAKE_BINARY_DIR}/eboot.bin" ${REI_VPK_DEPS}
  VERBATIM)
add_custom_target(vpk ALL DEPENDS "${REI_VPK}")

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
  src/frontend/ui.c
  src/frontend/touch.c
  src/common/re_pad.c
  src/frontend/update.c
  src/frontend/testpad.c
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

# kernel helper module (PS button, update install), in the VPK as module/rei_kctrl.skprx (as ReSys12). Its user stubs are
# made here directly (vita_create_stubs runs make on an unquoted path, and this tree's path has a space)
add_executable(rei_kctrl kmod/rei_kctrl.c)
set_target_properties(rei_kctrl PROPERTIES LINK_FLAGS "-nostdlib -fno-lto")
target_compile_options(rei_kctrl PRIVATE -fno-builtin -fno-lto) # (LTO would drop module_stop: only the exports table names it)
target_link_libraries(rei_kctrl SceCtrlForDriver_stub SceThreadmgrForDriver_stub SceIofilemgrForDriver_stub SceSysmemForDriver_stub)
# (vita_create_self splits its flags on spaces, so the export config path cannot be passed through it)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/rei_kctrl.skprx"
  COMMAND vita-elf-create -e "${CMAKE_SOURCE_DIR}/kmod/exports.yml" $<TARGET_FILE:rei_kctrl> "${CMAKE_BINARY_DIR}/rei_kctrl.velf"
  COMMAND vita-make-fself -c "${CMAKE_BINARY_DIR}/rei_kctrl.velf" "${CMAKE_BINARY_DIR}/rei_kctrl.skprx"
  DEPENDS rei_kctrl "${CMAKE_SOURCE_DIR}/kmod/exports.yml"
  VERBATIM)
add_custom_target(kctrl_skprx ALL DEPENDS "${CMAKE_BINARY_DIR}/rei_kctrl.skprx")
set(REI_KSTUB_DIR "${CMAKE_BINARY_DIR}/kctrl_stubs")
set(REI_KSTUB "${REI_KSTUB_DIR}/librei_kctrl_v1_stub_weak.a")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/kctrl.yml"
  COMMAND vita-elf-export kernel $<TARGET_FILE:rei_kctrl> "${CMAKE_SOURCE_DIR}/kmod/exports.yml" "${CMAKE_BINARY_DIR}/kctrl.yml"
  DEPENDS rei_kctrl "${CMAKE_SOURCE_DIR}/kmod/exports.yml"
  VERBATIM)
add_custom_command(OUTPUT "${REI_KSTUB}"
  COMMAND vita-libs-gen "${CMAKE_BINARY_DIR}/kctrl.yml" "${REI_KSTUB_DIR}"
  COMMAND make -C "${REI_KSTUB_DIR}"
  DEPENDS "${CMAKE_BINARY_DIR}/kctrl.yml"
  VERBATIM)
add_custom_target(kctrl_stubs DEPENDS "${REI_KSTUB}")

# RePad: Bluetooth controllers (kernel plugin, MIT; vita/repad = the RePad project's files, keep them the same), in the
# VPK as module/repad.skprx. The app loads it when it is not running yet (re_pad_load_plugin)
add_executable(repad repad/src/main.c repad/src/bt.c repad/src/ctrl.c repad/src/pads.c)
set_target_properties(repad PROPERTIES LINK_FLAGS "-nostdlib -fno-lto")
# (taihen's TAI_CONTINUE: an empty () must mean any args; no LTO: module_start is named only by the module config)
target_compile_options(repad PRIVATE -fno-builtin -std=gnu11 -fno-lto)
target_link_libraries(repad SceBtForDriver_stub SceCtrlForDriver_stub SceKernelSuspendForDriver_stub SceSysclibForDriver_stub
  SceThreadmgrForDriver_stub taihenForKernel_stub taihenModuleUtils_stub SceSblAIMgrForDriver_stub)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/repad.skprx"
  COMMAND vita-elf-create -e "${CMAKE_SOURCE_DIR}/repad/repad.yml" $<TARGET_FILE:repad> "${CMAKE_BINARY_DIR}/repad.velf"
  COMMAND vita-make-fself -c "${CMAKE_BINARY_DIR}/repad.velf" "${CMAKE_BINARY_DIR}/repad.skprx"
  DEPENDS repad "${CMAKE_SOURCE_DIR}/repad/repad.yml"
  VERBATIM)
add_custom_target(repad_skprx ALL DEPENDS "${CMAKE_BINARY_DIR}/repad.skprx")
add_dependencies(${PROJECT_NAME} kctrl_stubs)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/kmod)

target_link_libraries(${PROJECT_NAME}
  "${REI_KSTUB}"
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
  SceShellSvc_stub
  SceRtc_stub
  SceTouch_stub
  SceLibKernel_stub
  taihen_stub
  mbedtls
  mbedx509
  mbedcrypto
  pthread
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
# Bezel artwork is not packed in the VPK (copyright): users put it in ux0:data/reirem/artwork/<set>.png.
# module/rei_kctrl.skprx: the kernel helper; assets/blankArtwork.png: the border artwork of games without their own (shapes
# and text only, tools/make_app_art.py); cacert.pem: certificates for the update check (HTTPS)
set(REI_VPK_ADD_ARGS -a "${CMAKE_BINARY_DIR}/rei_kctrl.skprx=module/rei_kctrl.skprx"
  -a "${CMAKE_BINARY_DIR}/repad.skprx=module/repad.skprx"
  -a "${CMAKE_SOURCE_DIR}/sce_sys_src/blankArtwork.png=assets/blankArtwork.png"
  -a "${CMAKE_SOURCE_DIR}/res/cacert.pem=cacert.pem")
set(REI_VPK_DEPS "${CMAKE_BINARY_DIR}/rei_kctrl.skprx" "${CMAKE_BINARY_DIR}/repad.skprx" "${CMAKE_SOURCE_DIR}/sce_sys_src/blankArtwork.png"
  "${CMAKE_SOURCE_DIR}/res/cacert.pem")
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

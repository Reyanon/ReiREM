# ReiREM PSP port. Build: tools/build.sh psp  (-> build-psp/, needs PSPDEV)
# Toolchain: pspdev (https://github.com/pspdev/pspdev), -DCMAKE_TOOLCHAIN_FILE=$PSPDEV/psp/share/pspdev.cmake
cmake_minimum_required(VERSION 3.16)
project(reirem_psp C CXX)

set(REI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Release flags (psp/docs/RULES.md ch.5). -O3 may hurt I-cache; decide after measuring
set(CMAKE_C_FLAGS_RELEASE   "-O2 -G0 -ffast-math -fno-strict-aliasing")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -G0 -ffast-math -fno-strict-aliasing")
# Sound->main delay in lines (PSP-ADR-005)
set(REI_PSP_S2M_DELAY "192" CACHE STRING "Sound->main delay in lines")
# Experiment: extra per-file optimization (e.g. -DREI_PSP_YM_OPT="-O3"). YM2151 synthesis runs on the ME
set(REI_PSP_YM_OPT "" CACHE STRING "Extra compile options for ym2151.c only")

# Logging (PSPLink console printf): OFF by default in release builds (stripped at compile time). Enable: -DREI_LOG=ON
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(REI_LOG_DEFAULT OFF)
else()
  set(REI_LOG_DEFAULT ON)
endif()
option(REI_LOG "Include PSPLink console log" ${REI_LOG_DEFAULT})

# Emulator core (same sources/defines as Vita and PC harness)
# Other Irem boards included too (M92 uses the GE/ME path; other boards use FBNeo's original CPU-drawn frame, main.c run_other_board)
set(REI_OTHER_BOARDS ON CACHE BOOL "" FORCE)
# V33 dynamic recompiler (ADR-020). Code cache size in KB (small, main memory is tight)
option(REI_PSP_DRC "V33 dynamic recompiler" ON)
set(REI_DRC ${REI_PSP_DRC} CACHE BOOL "" FORCE)
set(REI_PSP_DRC_KB "256" CACHE STRING "dynarec code cache KB")
include(${REI_ROOT}/src/core/core.cmake)
if(REI_PSP_DRC)
  target_compile_definitions(rei_core PRIVATE REI_DRC_CACHE_KB=${REI_PSP_DRC_KB})
endif()
if(NOT REI_PSP_YM_OPT STREQUAL "")
  set_source_files_properties(${REI_ROOT}/src/core/burn/snd/ym2151.c PROPERTIES COMPILE_OPTIONS "${REI_PSP_YM_OPT}")
endif()

enable_language(ASM)
# Outside-screen artwork is not bundled (copyright): users put artwork/<set>.png in the app folder
# (src/font_ui.c: the menu font, Noto Sans pre-rendered by psp/tools/make_font.py — SIL OFL 1.1, psp/font/OFL.txt)
add_executable(reirem src/main.c src/video.c src/ge_layers.c src/ge_m107.c src/ge_m72.c src/atomic64.c src/settings.c src/menu.c src/ui.c
  src/font_ui.c)
target_link_libraries(reirem PRIVATE rei_core z pspgu pspdebug pspdisplay pspge pspctrl psppower psprtc pspdmac m)
# Loading screen background: the app's own art (vita/sce_sys/livearea/contents/bg.png, shapes and text only — tools/make_app_art.py),
# bundled into the EBOOT (#included by menu.c)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/rei_bg_png.c
  COMMAND ${PSPDEV}/bin/bin2c ${REI_ROOT}/vita/sce_sys/livearea/contents/bg.png ${CMAKE_CURRENT_BINARY_DIR}/rei_bg_png.c rei_bg_png
  DEPENDS ${REI_ROOT}/vita/sce_sys/livearea/contents/bg.png VERBATIM)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/rei_bg_png.c PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(reirem PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/rei_bg_png.c)
# Border artwork of games without their own: the app's blank artwork (vita/sce_sys_src/blankArtwork.png, shapes only, same as
# the Vita), bundled into the EBOOT (#included by video.c)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/rei_blank_png.c
  COMMAND ${PSPDEV}/bin/bin2c ${REI_ROOT}/vita/sce_sys_src/blankArtwork.png ${CMAKE_CURRENT_BINARY_DIR}/rei_blank_png.c rei_blank_png
  DEPENDS ${REI_ROOT}/vita/sce_sys_src/blankArtwork.png VERBATIM)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/rei_blank_png.c PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(reirem PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/rei_blank_png.c)
# Version (git commit hash, options screen About): rei_version.h regenerated every build (tools/git_version.cmake, shared with Vita)
target_include_directories(reirem PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
add_custom_target(rei_version
  COMMAND ${CMAKE_COMMAND} -DSRC=${REI_ROOT} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/rei_version.h -P ${REI_ROOT}/tools/git_version.cmake
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/rei_version.h
  VERBATIM)
add_dependencies(reirem rei_version)
target_compile_definitions(reirem PRIVATE REI_PSP_S2M_DELAY=${REI_PSP_S2M_DELAY})
# Tile count threshold for also drawing normal (non-strip) layer runs with per-key slot assignment (empty = strip runs only, video.c)
set(REI_PSP_SORT_MIN "64" CACHE STRING "Also sort normal runs with at least this many tiles")
if(NOT REI_PSP_SORT_MIN STREQUAL "")
  target_compile_definitions(reirem PRIVATE REI_PSP_SORT_MIN=${REI_PSP_SORT_MIN})
endif()

# Sound side on the Media Engine (psp/docs/DECISIONS.md PSP-ADR-003). Needs mcidclan psp-media-engine-custom-core
# (MIT) installed into pspdev (psp/docs/BUILD_ME.md). With REI_ME_SPLIT the core separates ME-written data and shared vars by cache line
# (src/core/shim/rei_me.h), and me.ld gathers that data
option(REI_PSP_ME "Run the sound side on the Media Engine" ON)
if(REI_PSP_ME)
  target_compile_definitions(rei_core PUBLIC REI_ME_SPLIT REI_YM_LAZY)
  # Other boards: YM2151 synthesis on the ME (along with ME drawing, sound 1 frame late — burn_ym2151.cpp REI_YM_ME)
  target_compile_definitions(rei_core PUBLIC REI_YM_ME)
  # YM2151 synthesis rate (0 = original 55.9kHz, 1 = half 28kHz: saves ~4-5ms on the sound side, highs differ slightly — approved)
  set(REI_PSP_YM_RATE_SHIFT "1" CACHE STRING "YM2151 synthesis rate reduction step")
  if(NOT REI_PSP_YM_RATE_SHIFT STREQUAL "0")
    target_compile_definitions(rei_core PUBLIC REI_YM_RATE_SHIFT=${REI_PSP_YM_RATE_SHIFT})
  endif() # REI_YM_SPLIT: split YM2151 channels (accurate, but currently a loss — PERF PP8)
  set(REI_PSP_YM_MAIN "0" CACHE STRING "YM2151 channel bits synthesized by the main CPU (0 = all on ME)")
  target_compile_definitions(reirem PRIVATE REI_PSP_YM_MAIN=${REI_PSP_YM_MAIN})
  if(NOT REI_PSP_YM_MAIN STREQUAL "0")
    target_compile_definitions(rei_core PUBLIC REI_YM_SPLIT) # main synthesizes some channels (same result)
  endif()
  target_compile_definitions(reirem PRIVATE REI_PSP_ME)
  target_sources(reirem PRIVATE src/me_sound.c src/overclock.c)
  # Overclock (PSP-2000/3000/go, direct PLL multiplier control — src/overclock.c). Off by default: on the dev unit (6.61 PRO family CFW), writing
  # the PLL multiplier register via kcall powered off even when rewriting the same value (PSP-ADR-006)
  set(REI_PSP_OC_MHZ "0" CACHE STRING "CPU clock MHz (above 333 = PLL overclock, 0 = off)")
  target_compile_definitions(reirem PRIVATE REI_PSP_OC_MHZ=${REI_PSP_OC_MHZ})
  # Diagnostic stage (0 = off): 1 register read only, 2 + wait with interrupts off, 3 + rewrite current multiplier, 4 + raise one step and restore
  set(REI_PSP_OC_DRY "0" CACHE STRING "Overclock diagnostic stage")
  target_compile_definitions(reirem PRIVATE REI_PSP_OC_DRY=${REI_PSP_OC_DRY})
  target_link_libraries(reirem PRIVATE me-core-lib me-core pspaudio pspkubridge)
  target_link_options(reirem PRIVATE -Wl,-L,${CMAKE_CURRENT_SOURCE_DIR} -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/me.ld)
  if(REI_PSP_DRC) # dynarec code cache = fast-path table region (me.ld .reifast_data 576KB, hooks disabled)
    target_compile_definitions(rei_core PRIVATE REI_DRC_FASTDATA)
    # Second region (heap, if room after core/display init): reduces recompilation
    set(REI_PSP_DRC_EXTRA_KB "256" CACHE STRING "dynarec code cache second region KB (0 = none)")
    if(NOT REI_PSP_DRC_EXTRA_KB STREQUAL "0")
      target_compile_definitions(rei_core PRIVATE REI_DRC_EXTRA_KB=${REI_PSP_DRC_EXTRA_KB})
    endif()
    # On first fill, grow the second region to this size (boards with large game code — M107 dsoccr94 in-game 3.3MB overflowed to the interpreter)
    set(REI_PSP_DRC_GROW_KB "8192" CACHE STRING "Size KB to grow the dynarec second region (0 = no growth, halves on failure) — dsoccr94 code exceeded 4MB on hardware")
    if(NOT REI_PSP_DRC_GROW_KB STREQUAL "0")
      target_compile_definitions(rei_core PRIVATE REI_DRC_GROW_KB=${REI_PSP_DRC_GROW_KB})
    endif()
  endif()
  # Experiment: padding before the module (before ME code) — check whether launch method changes module address and ME cache layout
  set(REI_PSP_ME_PAD "0" CACHE STRING "Padding bytes before ME code")
  if(NOT REI_PSP_ME_PAD STREQUAL "0")
    target_link_options(reirem PRIVATE -Wl,--defsym=REI_ME_PAD=${REI_PSP_ME_PAD})
  endif()
  # Put the V33 interpreter's hot code (dispatch loop, fetch, EA, frequent handlers) together up front (nec_hot.ld, PERF PP19)
  option(REI_PSP_HOT_ORDER "Order V33 handlers by execution frequency" ON)
  if(REI_PSP_HOT_ORDER)
    set_source_files_properties(${REI_ROOT}/src/core/cpu/nec/nec.cpp PROPERTIES COMPILE_OPTIONS "-ffunction-sections")
  endif()
  # Per-line code (line loop, line end, raster IRQ entry and fast paths) as one block after that (src/core/shim/rei_line.h)
  option(REI_PSP_LINE_ORDER "Group per-line path functions together" ON)
  if(REI_PSP_LINE_ORDER)
    target_compile_definitions(rei_core PRIVATE REI_LINE_ORDER)
  endif()
endif()
if(REI_LOG)
  target_compile_definitions(reirem PRIVATE REI_LOG_ENABLED)
endif()
# Diagnostic: do not send lists to the GE (measure without GE memory bus use; display freezes)
option(REI_PSP_NO_GE_TEST "Diag: do not submit GE lists" OFF)
if(REI_PSP_NO_GE_TEST)
  target_compile_definitions(reirem PRIVATE REI_PSP_NO_GE_TEST)
endif()
# Player build: pad control from the start (no autoplay), first-600-frame hash/GE checks off. OFF for the dev loop
option(REI_PSP_PLAYER "Player build" OFF)
if(REI_PSP_PLAYER)
  target_compile_definitions(reirem PRIVATE REI_PSP_PLAYER)
endif()
# (test) start a game from host0:/rei_game without the list even in no-log builds (to reproduce player builds)
option(REI_PSP_TESTGAME "Read host0:/rei_game (no-log build test)" OFF)
if(REI_PSP_TESTGAME)
  target_compile_definitions(reirem PRIVATE REI_PSP_TESTGAME)
endif()
set(REI_PSP_Z80_OPT "" CACHE STRING "(test) z80.cpp optimization options (e.g. -Os)")
if(NOT REI_PSP_Z80_OPT STREQUAL "")
  set_property(SOURCE ${REI_ROOT}/src/core/cpu/z80/z80.cpp APPEND PROPERTY COMPILE_OPTIONS ${REI_PSP_Z80_OPT})
endif()
# Diagnostic: function-level sampling profile (gcc -pg + pspsdk libpspprof). The other-board loop measures 300-900 frames and
# writes host0:/gmon.out (main.c REI_PSP_GPROF) -> psp-gprof build-psp-prof/reirem gmon.out
option(REI_PSP_GPROF "Function-level profile (diag)" OFF)
if(REI_PSP_GPROF)
  target_compile_definitions(reirem PRIVATE REI_PSP_GPROF)
  target_compile_options(rei_core PRIVATE -pg)
  target_compile_options(reirem PRIVATE -pg)
  target_link_options(reirem PRIVATE -pg)
  target_link_libraries(reirem PRIVATE pspprof)
endif()
# Diagnostic: count soft double ops (libgcc __muldf3 ...) and call sites (main.c sf_*, profile build). PSP FPU is
# single precision only, so double ops are function calls
option(REI_SFCOUNT "Count soft double calls (diag)" OFF)
if(REI_SFCOUNT)
  target_compile_definitions(reirem PRIVATE REI_SFCOUNT)
  foreach(f __muldf3 __adddf3 __subdf3 __divdf3 __floatsidf __floatunsidf __fixdfsi __fixunsdfsi __extendsfdf2 __truncdfsf2
            __ltdf2 __gtdf2 __ledf2 __gedf2 __eqdf2 __nedf2 __fixdfdi __fixunsdfdi __floatdidf __floatundidf)
    target_link_options(reirem PRIVATE -Wl,--wrap=${f})
  endforeach()
endif()
# Diagnostic: section profiler time source = hardware counter number (main.c hw_counter, run.sh PSP_HWPROF=1). Empty = off
set(REI_HWPROF "" CACHE STRING "Hardware counter number (2 cpuck, 13 i_miss, 14 d_miss ...)")
if(NOT REI_HWPROF STREQUAL "")
  target_compile_definitions(reirem PRIVATE REI_HWPROF=${REI_HWPROF})
  # stub calling nbbk_counter() in the nbbkprof kernel module
  enable_language(ASM)
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/nbbkprof_stub.S
    COMMAND ${PSPDEV}/bin/psp-build-exports -s ${CMAKE_CURRENT_SOURCE_DIR}/devstub/kprof_exports.exp
    COMMAND ${CMAKE_COMMAND} -E rename ${CMAKE_CURRENT_BINARY_DIR}/nbbkprof.S ${CMAKE_CURRENT_BINARY_DIR}/nbbkprof_stub.S
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/devstub/kprof_exports.exp)
  target_sources(reirem PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/nbbkprof_stub.S)
endif()

# BUILD_PRX: put a PRX in the EBOOT instead of the elf. ENC_PRX: encrypt that PRX — unencrypted, launching
# from the XMB failed with "8002014E" (2026-10-09 on hardware)
# REI_PSP_ENC=OFF: skip encryption. With V33 recompilation (REI_RECOMP) code reaches 19MB and PrxEncrypter dies with
# "buffer overflow" — the dev loop (PSPLink) uses only the plain PRX, so turn it off then
option(REI_PSP_ENC "Encrypt the EBOOT's PRX (for XMB launch)" ON)
set(REI_PSP_TITLE "ReiREM" CACHE STRING "XMB title (change to install a comparison build separately)")
if(REI_PSP_ENC)
  create_pbp_file(
    TARGET reirem
    TITLE "${REI_PSP_TITLE}"
    ICON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/ICON0.PNG       # XMB icon 144x80 (tools/make_app_art.py; no PIC1 background)
    MEMSIZE 1 # PSP-3000/go extended memory (~52MB). Macro default 2 (limited) made ROM loading fail in the 24MB region
    BUILD_PRX
    ENC_PRX
  )
else()
  create_pbp_file(
    TARGET reirem
    TITLE "${REI_PSP_TITLE}"
    ICON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/ICON0.PNG
    MEMSIZE 1
    BUILD_PRX
  )
endif()

# ENC_PRX encrypts reirem.prx in place. Encrypted PRX cannot be loaded by PSPLink (0x80020148),
# so build a plain PRX from the same elf -> psp/tools/run.sh runs reirem.plain.prx
add_custom_command(TARGET reirem POST_BUILD
  COMMAND ${PSPDEV}/bin/psp-prxgen $<TARGET_FILE:reirem> $<TARGET_FILE:reirem>.plain.prx
  VERBATIM)

# Keep the unstripped .elf as artifacts/psp-<commit>.elf each build (resolve PSPLink exception addrs with psp-addr2line)
add_custom_command(TARGET reirem POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DELF=$<TARGET_FILE:reirem> -DSRC=${REI_ROOT} -DPREFIX=psp-
          -P ${REI_ROOT}/tools/archive_elf.cmake
  VERBATIM)

# Empty game for PSPLink development (psp/devstub)
add_subdirectory(devstub)

# ReiREM emulator core source list - shared by the Vita build and the PC harness.
# Usage: include(${REI_ROOT}/src/core/core.cmake), then link the rei_core static library.

set(REI_CORE_DIR ${CMAKE_CURRENT_LIST_DIR})

set(REI_CORE_SOURCES
  # FBNeo originals (ADR-002 A)
  ${REI_CORE_DIR}/burn/drv/irem/d_m92.cpp
  ${REI_CORE_DIR}/burn/drv/irem/irem_cpu.cpp
  ${REI_CORE_DIR}/cpu/nec_intf.cpp
  ${REI_CORE_DIR}/cpu/nec/nec.cpp      # includes necinstr.c
  ${REI_CORE_DIR}/cpu/nec/v25.cpp      # includes v25sfr.c, v25instr.c, necinstr.c
  ${REI_CORE_DIR}/burn/snd/burn_ym2151.cpp
  ${REI_CORE_DIR}/burn/snd/ym2151.c
  ${REI_CORE_DIR}/burn/snd/ym2151_main.c
  ${REI_CORE_DIR}/burn/snd/iremga20.cpp
  ${REI_CORE_DIR}/burn/snd/msm6295.cpp
  ${REI_CORE_DIR}/burn/devices/pic8259.cpp
  ${REI_CORE_DIR}/burn/timer.cpp
  ${REI_CORE_DIR}/burn/burn_sound.cpp
  ${REI_CORE_DIR}/burn/burn_sound_c.cpp
  ${REI_CORE_DIR}/burn/burn_memory.cpp
  # FBNeo shared drawing (also used by M92)
  ${REI_CORE_DIR}/burn/tiles_generic.cpp
  ${REI_CORE_DIR}/burn/tilemap_generic.cpp
  ${REI_CORE_DIR}/burn/burn_bitmap.cpp
  ${REI_CORE_DIR}/burn/burn_pal.cpp
  # Replacement code (ADR-002 B)
  ${REI_CORE_DIR}/shim/burn_shim.cpp
  ${REI_CORE_DIR}/shim/zipfile.c
  ${REI_CORE_DIR}/shim/rei_games.c
  ${REI_CORE_DIR}/shim/rei_cheats.cpp
  ${REI_CORE_DIR}/shim/rei_defer.cpp
)

# Other Irem boards (ADR-018): drivers and FBNeo parts used only by them. Off on PSP, which still runs M92 only (memory)
set(REI_CORE_OTHER_SOURCES
  ${REI_CORE_DIR}/burn/drv/irem/d_m107.cpp
  ${REI_CORE_DIR}/burn/drv/irem/d_m90.cpp
  ${REI_CORE_DIR}/burn/drv/irem/d_m72.cpp
  ${REI_CORE_DIR}/burn/drv/irem/d_vigilant.cpp
  ${REI_CORE_DIR}/burn/drv/irem/d_m62.cpp
  ${REI_CORE_DIR}/burn/drv/irem/d_m63.cpp
  ${REI_CORE_DIR}/burn/drv/irem/d_m58.cpp
  ${REI_CORE_DIR}/burn/drv/irem/d_m57.cpp
  ${REI_CORE_DIR}/burn/drv/irem/d_m52.cpp
  ${REI_CORE_DIR}/burn/drv/irem/d_travrusa.cpp
  ${REI_CORE_DIR}/burn/drv/irem/irem_sound.cpp
  ${REI_CORE_DIR}/burn/burn_gun.cpp
  ${REI_CORE_DIR}/burn/devices/resnet.cpp
  ${REI_CORE_DIR}/cpu/z80_intf.cpp
  ${REI_CORE_DIR}/cpu/z80/z80.cpp
  ${REI_CORE_DIR}/cpu/z80/z80daisy.cpp
  ${REI_CORE_DIR}/cpu/z80/z80ctc.cpp
  ${REI_CORE_DIR}/cpu/z80/z80pio.cpp
  ${REI_CORE_DIR}/cpu/m6800_intf.cpp
  ${REI_CORE_DIR}/cpu/m6800/m6800.cpp
  ${REI_CORE_DIR}/cpu/i8039/i8039.cpp
  ${REI_CORE_DIR}/cpu/i8051/mcs51.cpp
  ${REI_CORE_DIR}/burn/snd/ay8910.c
  ${REI_CORE_DIR}/burn/snd/msm5205.cpp
  ${REI_CORE_DIR}/burn/snd/dac.cpp
  ${REI_CORE_DIR}/burn/snd/samples.cpp
  ${REI_CORE_DIR}/burn/snd/burn_ym2203.cpp
  ${REI_CORE_DIR}/burn/snd/fm.c
  ${REI_CORE_DIR}/burn/snd/ymdeltat.c
  ${REI_CORE_DIR}/burn/devices/joyprocess.cpp
)
option(REI_OTHER_BOARDS "Include Irem board drivers other than M92" ON)
if(REI_OTHER_BOARDS)
  list(APPEND REI_CORE_SOURCES ${REI_CORE_OTHER_SOURCES})
endif()

add_library(rei_core STATIC ${REI_CORE_SOURCES})
if(NOT REI_OTHER_BOARDS)
  target_compile_definitions(rei_core PUBLIC REI_M92_ONLY)
endif()

# shim first so tchar.h resolves to the shim version
target_include_directories(rei_core PUBLIC
  ${REI_CORE_DIR}/shim
  ${REI_CORE_DIR}/burn
  ${REI_CORE_DIR}/burn/snd
  ${REI_CORE_DIR}/burn/devices
  ${REI_CORE_DIR}/burn/drv/irem
  ${REI_CORE_DIR}/cpu
  ${REI_CORE_DIR}/cpu/nec
  ${REI_CORE_DIR}/cpu/z80
  ${REI_CORE_DIR}/cpu/m6800
  ${REI_CORE_DIR}/cpu/i8039
  ${REI_CORE_DIR}/cpu/i8051
)

# __fastcall is an x86-only calling convention: define empty (as in the FBNeo libretro build; nec_intf.h/z80_intf.h also define it empty)
target_compile_definitions(rei_core PRIVATE LSB_FIRST __fastcall=)

# Experimental: build only the core at a different optimization level (e.g. -DREI_CORE_OPT="-O2"). Empty = follow global flags
set(REI_CORE_OPT "" CACHE STRING "Extra compile options for the core only (appended after global flags)")
if(REI_CORE_OPT)
  separate_arguments(REI_CORE_OPT_LIST UNIX_COMMAND "${REI_CORE_OPT}")
  target_compile_options(rei_core PRIVATE ${REI_CORE_OPT_LIST})
endif()

# PC histogram (analysis only, meaningful only in the PC harness)
option(REI_PCHIST "Collect per-CPU PC histograms" OFF)
if(REI_PCHIST)
  target_compile_definitions(rei_core PUBLIC REI_PCHIST)
endif()

# M6 V33 static recompilation (generated from the user's ROM by tools/recomp/gen.sh, never committed). Enable with -DREI_RECOMP=ON
set(REI_RC_DIR ${REI_CORE_DIR}/../recomp/generated)
# On hold (ADR-011), so OFF by default. Builds slowly when on (Vita Release ~18 min, 3.4GB memory)
option(REI_RECOMP "Use V33 static recompiled code" OFF)
if(REI_RECOMP)
  if(NOT EXISTS ${REI_RC_DIR}/v33_rc.inc)
    message(FATAL_ERROR "REI_RECOMP: ${REI_RC_DIR}/v33_rc.inc not found (run tools/recomp/gen.sh)")
  endif()
  target_compile_definitions(rei_core PUBLIC REI_RECOMP)
  target_include_directories(rei_core PRIVATE ${REI_RC_DIR})
  set_source_files_properties(${REI_CORE_DIR}/cpu/nec/nec.cpp PROPERTIES OBJECT_DEPENDS ${REI_RC_DIR}/v33_rc.inc)
endif()

# Section profiler (shim/rei_prof.h). When off, all instrumentation is compiled out
# V33 dynamic recompiler (ADR-020): MIPS only (PSP, qemu-mipsel harness for verification). Also enables the instruction fetch page cache
# (REI_FETCH_CACHE) so the interpreter path matches PSP (uses the map change notifier rei_nec_fetch_flush)
option(REI_DRC "V33 dynamic recompiler (MIPS)" OFF)
if(REI_DRC)
  target_compile_definitions(rei_core PUBLIC REI_DRC REI_FETCH_CACHE)
endif()

option(REI_PROFILE "Core section profiling instrumentation" OFF)
option(REI_PROF_FRAME_ONLY "(diag) Skip CPU sections in profile builds (for hardware counter measurements)" OFF)
if(REI_PROFILE)
  target_compile_definitions(rei_core PUBLIC REI_PROFILE)
  if(REI_PROF_FRAME_ONLY)
    target_compile_definitions(rei_core PUBLIC REI_PROF_FRAME_ONLY)
  endif()
endif()
# Warnings in original FBNeo code are not fixed, so silence them (keep originals unchanged)
target_compile_options(rei_core PRIVATE -w -fno-strict-aliasing -fsigned-char)

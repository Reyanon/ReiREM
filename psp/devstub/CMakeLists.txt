# REIREM DEV: empty game EBOOT for PSPLink development (-> build-psp/devstub/EBOOT.PBP). run.sh copies it to Memory Stick PSP/GAME/REIREMDEV
add_executable(reiremdev main.c)
target_link_libraries(reiremdev PRIVATE pspdebug pspdisplay pspge pspctrl psppower)
create_pbp_file(
  TARGET reiremdev
  TITLE "REIREM DEV - PSPLink host"
  MEMSIZE 1 # PSP-3000/go extended memory (~52MB). The macro default 2 (limited) made ROM loading fail in the 24MB area
  BUILD_PRX
  ENC_PRX
)

# nbblaunch: kernel PRX that launches REIREM DEV from the XMB (-> build-psp/devstub/nbblaunch.prx). run.sh loads it via PSPLink
add_executable(nbblaunch launch.c)
target_link_options(nbblaunch PRIVATE -nostartfiles
  -specs=${PSPDEV}/psp/sdk/lib/prxspecs -Wl,-q,-T${PSPDEV}/psp/sdk/lib/linkfile.prx ${PSPDEV}/psp/sdk/lib/prxexports.o)
target_link_libraries(nbblaunch PRIVATE pspsystemctrl_kernel pspkernel)
add_custom_command(TARGET nbblaunch POST_BUILD
  COMMAND ${PSPDEV}/bin/psp-fixup-imports $<TARGET_FILE:nbblaunch>
  COMMAND ${PSPDEV}/bin/psp-prxgen $<TARGET_FILE:nbblaunch> $<TARGET_FILE:nbblaunch>.prx
  VERBATIM)
# nbblaunch_play: same launcher, starts the play install (PSP/GAME/REIREM) (for exit/performance checks)
add_executable(nbblaunch_play launch.c)
target_compile_definitions(nbblaunch_play PRIVATE REI_LAUNCH_GAME="REIREM")
target_link_options(nbblaunch_play PRIVATE -nostartfiles
  -specs=${PSPDEV}/psp/sdk/lib/prxspecs -Wl,-q,-T${PSPDEV}/psp/sdk/lib/linkfile.prx ${PSPDEV}/psp/sdk/lib/prxexports.o)
target_link_libraries(nbblaunch_play PRIVATE pspsystemctrl_kernel pspkernel)
add_custom_command(TARGET nbblaunch_play POST_BUILD
  COMMAND ${PSPDEV}/bin/psp-fixup-imports $<TARGET_FILE:nbblaunch_play>
  COMMAND ${PSPDEV}/bin/psp-prxgen $<TARGET_FILE:nbblaunch_play> $<TARGET_FILE:nbblaunch_play>.prx
  VERBATIM)

# reilaunch: launches the EBOOT given as argument, used when the app relaunches itself via Select game in the options screen (bundled into the EBOOT).
# User-mode sceKernelLoadExec dropped to the XMB on this CFW
add_executable(reilaunch launch.c)
target_compile_definitions(reilaunch PRIVATE REI_LAUNCH_ARG)
target_link_options(reilaunch PRIVATE -nostartfiles
  -specs=${PSPDEV}/psp/sdk/lib/prxspecs -Wl,-q,-T${PSPDEV}/psp/sdk/lib/linkfile.prx ${PSPDEV}/psp/sdk/lib/prxexports.o)
target_link_libraries(reilaunch PRIVATE pspsystemctrl_kernel pspkernel)
add_custom_command(TARGET reilaunch POST_BUILD
  COMMAND ${PSPDEV}/bin/psp-fixup-imports $<TARGET_FILE:reilaunch>
  COMMAND ${PSPDEV}/bin/psp-prxgen $<TARGET_FILE:reilaunch> $<TARGET_FILE:reilaunch>.prx
  VERBATIM)

# nbbkprof: enable performance counters + export nbbk_counter() (dev only, PSP_HWPROF=1 run.sh, app REI_HWPROF build)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/kprof_exports.c
  COMMAND ${PSPDEV}/bin/psp-build-exports -b ${CMAKE_CURRENT_SOURCE_DIR}/kprof_exports.exp > ${CMAKE_CURRENT_BINARY_DIR}/kprof_exports.c
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kprof_exports.exp)
add_executable(nbbkprof kunlock.c ${CMAKE_CURRENT_BINARY_DIR}/kprof_exports.c)
target_link_options(nbbkprof PRIVATE -nostartfiles
  -specs=${PSPDEV}/psp/sdk/lib/prxspecs -Wl,-q,-T${PSPDEV}/psp/sdk/lib/linkfile.prx)
target_link_libraries(nbbkprof PRIVATE pspkernel)
# No imported functions, so psp-fixup-imports is skipped ("no .lib.stub section")
add_custom_command(TARGET nbbkprof POST_BUILD
  COMMAND ${PSPDEV}/bin/psp-prxgen $<TARGET_FILE:nbbkprof> $<TARGET_FILE:nbbkprof>.prx
  VERBATIM)

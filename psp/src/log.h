// PSP log: when run via PSPLink, printf output goes to the pspsh/usbhostfs_pc console (psp/docs/RULES.md ch. 7).
// When run as a Memory Stick EBOOT, output is discarded. Compiled out in release builds (REI_LOG=OFF).
#ifndef REI_PSP_LOG_H
#define REI_PSP_LOG_H

#ifdef REI_LOG_ENABLED
#include <stdio.h>
#define REI_LOG(...) (printf(__VA_ARGS__), fflush(stdout))
#else
#define REI_LOG(...) ((void)0)
#endif

#endif

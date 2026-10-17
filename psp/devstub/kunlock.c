// nbbkprof: dev-only kernel module that enables the hardware performance counters (0xBC400000, pspdebug.h PspDebugProfilerRegs) and exports
// nbbk_counter() so user-mode apps can read them via syscall (MIPS user mode cannot read above 0x80000000 directly).
// PSP_HWPROF=1 bash psp/tools/run.sh loads it before the app; the app's REI_HWPROF build uses it as the time source for the core section profiler.
#include <pspkernel.h>

PSP_MODULE_INFO("nbbkprof", PSP_MODULE_KERNEL, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();

#define HW(a) (*(volatile unsigned int *)(a))

unsigned int nbbk_counter(int idx)
{
	if (idx < 1 || idx > 19)
		return 0;
	return HW(0xbc400000 + 4 * idx);
}

int module_start(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	HW(0xbc400000) = 1; // enable counter
	__asm__ volatile("sync");
	return 0; // stay resident
}

int module_stop(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	return 0;
}

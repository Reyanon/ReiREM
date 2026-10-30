// nbblaunch - kernel module that starts REIREM DEV from the XMB (loaded via PSPLink by psp/tools/run.sh).
// PSPLink modexec does not work from this CFW's XMB, so call SystemControl's
// sctrlKernelLoadExecVSHMs2 (Memory Stick) / Ef2 (PSP go internal) directly, as the XMB does for homebrew.
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psploadexec_kernel.h>
#include <systemctrl.h>

PSP_MODULE_INFO("nbblaunch", PSP_MODULE_KERNEL, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();

#ifndef REI_LAUNCH_GAME
#define REI_LAUNCH_GAME "REIREMDEV" // nbblaunch_play: "REIREM" (player install)
#endif
static char ms_path[] = "ms0:/PSP/GAME/" REI_LAUNCH_GAME "/EBOOT.PBP";
static char ef_path[] = "ef0:/PSP/GAME/" REI_LAUNCH_GAME "/EBOOT.PBP";

static void fill(SceKernelLoadExecVSHParam *p, char *path, SceSize len)
{
	char *b = (char *)p;
	for (unsigned i = 0; i < sizeof(*p); i++)
		b[i] = 0;
	p->size = sizeof(*p);
	p->args = len;
	p->argp = path;
	p->key = "game";
}

static char arg_path[160]; // reilaunch: EBOOT path from module_start args (the app relaunches itself to return to the game list)

static int launch_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	SceKernelLoadExecVSHParam p;
	if (arg_path[0]) {
		fill(&p, arg_path, sizeof(arg_path));
		if (arg_path[0] == 'e')
			sctrlKernelLoadExecVSHEf2(arg_path, &p);
		else
			sctrlKernelLoadExecVSHMs2(arg_path, &p);
		return sceKernelExitDeleteThread(0);
	}
	// PSP go: the install may be on internal storage (ef0:) even with a Memory Stick (ms0:) inserted; launch whichever has the file
	SceIoStat st;
	if (sceIoGetstat(ms_path, &st) >= 0) {
		fill(&p, ms_path, sizeof(ms_path));
		if (sctrlKernelLoadExecVSHMs2(ms_path, &p) >= 0)
			return sceKernelExitDeleteThread(0);
	}
	fill(&p, ef_path, sizeof(ef_path));
	sctrlKernelLoadExecVSHEf2(ef_path, &p);
	return sceKernelExitDeleteThread(0);
}

int module_start(SceSize args, void *argp)
{
#ifdef REI_LAUNCH_ARG
	if (args > 1 && argp) { // reilaunch: last string of the args (app passes just the EBOOT path, PSPLink ldstart passes args after the module path)
		const char *a = (const char *)argp;
		unsigned start = 0;
		for (unsigned i = 0; i + 1 < args; i++)
			if (a[i] == 0 && a[i + 1] != 0)
				start = i + 1;
		unsigned k = 0;
		for (unsigned i = start; i < args && a[i] && k < sizeof(arg_path) - 1; i++)
			arg_path[k++] = a[i];
		arg_path[k] = 0;
		static const char tail[] = "EBOOT.PBP";
		int ok = k >= 9;
		for (unsigned i = 0; ok && i < 9; i++)
			ok = arg_path[k - 9 + i] == tail[i];
		if (!ok)
			arg_path[0] = 0; // (unused unless it is an EBOOT path)
	}
#else
	(void)args;
	(void)argp;
#endif
	SceUID th = sceKernelCreateThread("nbblaunch", launch_thread, 0x20, 0x1000, 0, NULL);
	if (th >= 0)
		sceKernelStartThread(th, 0, NULL);
	return 0;
}

int module_stop(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	return 0;
}

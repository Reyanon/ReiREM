// REIREM DEV - empty game for PSPLink development (psp/docs/RULES.md ch.7, psp/tools/run.sh).
// With PSPLink as a plugin, modules cannot be loaded from the XMB (it competes for GE/memory). Holding game mode with
// this EBOOT lets run.sh reset (relaunch this EBOOT to free memory) and then load host0:'s reirem.plain.prx.
// Uses almost no memory. START exits.
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>

PSP_MODULE_INFO("reiremdev", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

int main(void)
{
	pspDebugScreenInit();
	pspDebugScreenPrintf("REIREM DEV - PSPLink host for ReiREM\n\n");
	pspDebugScreenPrintf("On the PC: bash psp/tools/run.sh\n\n");
	pspDebugScreenPrintf("START: exit\n");
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
	unsigned n = 0;
	for (;;) {
		SceCtrlData pad;
		sceCtrlReadBufferPositive(&pad, 1);
		if (pad.Buttons & PSP_CTRL_START)
			break;
		sceDisplayWaitVblankStart();
		if (++n % 60 == 0)
			scePowerTick(PSP_POWER_TICK_ALL); // prevent power save (screen off)
	}
	sceKernelExitGame();
	return 0;
}

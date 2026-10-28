// ReiREM kernel helper (ported from the ReSys12 / ReST-V one): the PS button for the app. The user side cannot see the PS button while the shell
// lock holds it (no sceCtrlPeekBuffer* reported it on the device), and a kernel read made inside the app's syscall
// only saw a long press, so a kernel thread of this module reads the pad every 8 ms, counts presses and double presses
// (home only on a double press), and on request presses PS for the system (button emulation) to go home.
// The module name carries a version (rei_kctrl_v1): a loaded module stays until reboot, and a new export under an
// old name would never be linked.
#include <psp2kern/ctrl.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/io/dirent.h>
#include <psp2kern/kernel/sysmem/uid_puid.h>

#define POLL_US 8000
#define DOUBLE_POLLS 50 // a second press within 400 ms is a double press
#define QUIET_POLLS 40  // after pressing PS for the system: its own press is not counted

static volatile unsigned int s_presses, s_doubles;
static volatile int s_held, s_run, s_home;
static SceUID s_thread = -1;

// An update into the app. Nothing can write the app folder while the app runs (not even another SELF of the
// same title, a syscall for it or this module's thread: EACCES), so the app unpacks the files, asks for the install
// and exits; this module's thread copies ux0:data/reirem/update/files/<rel> to ux0:app/M92A00001/<rel> once the folder
// can be written (the user starts the app again). One file: written next to the old one (.new), then put in its place
#define UPD_SRC "ux0:data/reirem/update/files/"
#define UPD_DST "ux0:app/M92A00001/"
static char s_cbuf[64 * 1024];
static int kcat(char *d, int dsz, const char *a, const char *b, const char *c)
{
	int n = 0;
	for (const char *s = a; *s && n < dsz - 1; s++) d[n++] = *s;
	for (const char *s = b; *s && n < dsz - 1; s++) d[n++] = *s;
	for (const char *s = c; *s && n < dsz - 1; s++) d[n++] = *s;
	d[n] = 0;
	return n < dsz - 1;
}
static int install_rel(const char *rel)
{
	int r = -1;
	static char src[256], dst[256], tmp[260]; // (one install at a time, on the module's thread; its stack is small)
	if (!rel[0] || rel[0] == '/')
		goto out;
	for (int i = 0; rel[i]; i++)
		if (rel[i] == ':' || rel[i] == '\\' || (rel[i] == '.' && rel[i + 1] == '.'))
			goto out;
	if (!kcat(src, sizeof(src), UPD_SRC, rel, "") || !kcat(dst, sizeof(dst), UPD_DST, rel, "") ||
		!kcat(tmp, sizeof(tmp), UPD_DST, rel, ".new"))
		goto out;
	for (int i = (int)sizeof(UPD_DST) - 1; dst[i]; i++) // the folders on the way
		if (dst[i] == '/') {
			dst[i] = 0;
			ksceIoMkdir(dst, 0777);
			dst[i] = '/';
		}
	const SceUID in = ksceIoOpen(src, SCE_O_RDONLY, 0);
	if (in < 0) {
		r = in;
		goto out;
	}
	const SceUID o = ksceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (o < 0) {
		ksceIoClose(in);
		r = o;
		goto out;
	}
	int n, ok = 1;
	while ((n = ksceIoRead(in, s_cbuf, sizeof(s_cbuf))) > 0)
		if (ksceIoWrite(o, s_cbuf, n) != n) {
			ok = 0;
			break;
		}
	ksceIoClose(in);
	ksceIoClose(o);
	if (!ok || n < 0) {
		ksceIoRemove(tmp);
		r = -2;
		goto out;
	}
	ksceIoRemove(dst);
	r = ksceIoRename(tmp, dst);
out:
	return r;
}

static volatile int s_later; // install asked for (the app is exiting)
static void install_dir(const char *rel, int depth, int *bad, int *n) // every file below update/files/<rel>
{
	char dir[256];
	if (!kcat(dir, sizeof(dir), rel[0] ? UPD_SRC : "ux0:data/reirem/update/files", rel, ""))
		return;
	const SceUID d = ksceIoDopen(dir);
	if (d < 0)
		return;
	SceIoDirent e;
	while (ksceIoDread(d, &e) > 0) {
		if (e.d_name[0] == '.')
			continue;
		char r[192];
		if (!kcat(r, sizeof(r), rel, rel[0] ? "/" : "", e.d_name))
			continue;
		if (SCE_S_ISDIR(e.d_stat.st_mode)) {
			if (depth < 6)
				install_dir(r, depth + 1, bad, n);
		} else {
			(*n)++;
			if (install_rel(r) < 0)
				(*bad)++;
		}
	}
	ksceIoDclose(d);
}
static void install_result(int n, int bad, int launched) // ux0:data/reirem/update_result.txt (the app logs it)
{
	char t[64];
	int k = 0;
	const int v[3] = { n, bad, launched };
	for (int i = 0; i < 3; i++) {
		unsigned x = (unsigned)v[i];
		char tmp[12];
		int m = 0;
		do { tmp[m++] = (char)('0' + x % 10); x /= 10; } while (x && m < 11);
		while (m) t[k++] = tmp[--m];
		t[k++] = i < 2 ? ' ' : 10;
	}
	const SceUID f = ksceIoOpen("ux0:data/reirem/update_result.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (f >= 0) {
		ksceIoWrite(f, t, k);
		ksceIoClose(f);
	}
}

static int poll_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	int held = 0, since = 1 << 20, quiet = 0;
	while (s_run) {
		if (s_later && ++s_later % 25 == 0) { // every 200 ms until the app's folder can be written
			if (install_rel("eboot.bin") >= 0) {
				int bad = 0, n = 0;
				install_dir("", 0, &bad, &n);
				install_result(n, bad, 0); // (the user starts the app again: no launch from the kernel)
				s_later = 0;
			} else if (s_later > 25 * 150) { // 30 s: given up
				install_result(0, -1, 0);
				s_later = 0;
			}
		}
		if (s_home) {
			s_home = 0;
			ksceCtrlSetButtonEmulation(0, 0, 0, SCE_CTRL_PSBUTTON, 8);
			quiet = QUIET_POLLS;
		}
		SceCtrlData d;
		d.buttons = 0;
		if (ksceCtrlPeekBufferPositive(0, &d, 1) >= 0) {
			const int ps = (d.buttons & SCE_CTRL_PSBUTTON) != 0;
			if (ps && !held && !quiet) {
				if (since < DOUBLE_POLLS) {
					s_doubles++;
					since = 1 << 20; // (a third press starts over)
				} else {
					s_presses++;
					since = 0;
				}
			}
			held = ps;
			s_held = ps;
		}
		if (since < (1 << 20))
			since++;
		if (quiet)
			quiet--;
		ksceKernelDelayThread(POLL_US);
	}
	return 0;
}

// PS button: bits 0-14 single presses, 15-29 double presses (both counted since the module started), bit 30 held now
unsigned int reiKCtrlPsState(void)
{
	int state;
	ENTER_SYSCALL(state);
	const unsigned int c = (s_presses & 0x7FFF) | ((s_doubles & 0x7FFF) << 15) | (s_held ? 1u << 30 : 0u);
	EXIT_SYSCALL(state);
	return c;
}

// press PS for the system once (the app has let go of its PS lock first): the home screen
int reiKCtrlGoHome(void)
{
	int state;
	ENTER_SYSCALL(state);
	s_home = 1;
	EXIT_SYSCALL(state);
	return 0;
}

// install the unpacked update once the app has exited (see install_dir)
int reiKCtrlInstallOnExit(void)
{
	int state;
	ENTER_SYSCALL(state);
	s_later = 1;
	EXIT_SYSCALL(state);
	return 0;
}

int module_start(SceSize args, void *argp);
int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	s_run = 1;
	s_thread = ksceKernelCreateThread("rei_kctrl", poll_thread, 0x3C, 0x4000, 0, 0, NULL); // (the install walks folders)
	if (s_thread >= 0)
		ksceKernelStartThread(s_thread, 0, NULL);
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	s_run = 0;
	if (s_thread >= 0) {
		ksceKernelWaitThreadEnd(s_thread, NULL, NULL);
		ksceKernelDeleteThread(s_thread);
	}
	return SCE_KERNEL_STOP_SUCCESS;
}

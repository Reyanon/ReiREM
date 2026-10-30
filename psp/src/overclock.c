// PSP overclock (PSP-2000/3000/go): raise the system PLL multiplier directly to push CPU/ME/bus clocks above 333MHz.
// scePower only accepts up to 333MHz, so start at 333MHz and raise the PLL multiplier register in 10MHz steps.
// Procedure (registers and order) written from scratch with reference to ARK-5 Compat/PSP overclock.c and its origin, mcidclan psp-undocumented-sorcery
// experimental-overclock (no code copied). Runs in kernel mode (ME library kcall).
//
// Registers (PSP-2000/3000/go values; 4000 and later differ):
//   0xbc1000fc  PLL multiplier: [31:16] upper (0x0124), [15:8] numerator, [7:0] denominator (20). Output = 37MHz × num / den
//   0xbc100068  PLL ratio select: low 4 bits index, bit7 = changing
//   0xbc200000  CPU domain ratio [24:16]/[8:0], 0xbc200004 bus domain ratio (both to 0x1ff/0x1ff)
#include "overclock.h"

#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>
#include <kubridge.h>
#include <me-core-mapper/me-lib.h>

#include "log.h"

#define OC_BASE_MHZ 37
#define OC_DEN 20
#define OC_MSB 0x0124u
#define OC_RATIO 5u
#define OC_DEFAULT 333
#define OC_STEP 10

#define HW(a) (*(volatile unsigned int *)(a))

static inline void hw_sync(void)
{
	__asm__ volatile("sync" ::: "memory");
}

// Wait for the clock to settle (0x2ffff spins). Touch only registers, no memory, while the PLL changes
// (the first version, counting in a stack variable, hung here)
static inline void oc_settle(void)
{
	__asm__ volatile(".set push\n .set noreorder\n .set nomacro\n .set noat\n"
			 " sync\n lui $t0, 0x02\n ori $t0, $t0, 0xffff\n"
			 "1: nop\n nop\n nop\n nop\n nop\n nop\n nop\n addiu $t0, $t0, -1\n bnez $t0, 1b\n nop\n"
			 ".set pop\n" ::: "$t0", "memory");
}

static inline unsigned int oc_intr_off(void)
{
	unsigned int st;
	__asm__ volatile("mfc0 %0, $12\n sync" : "=r"(st));
	const unsigned int off = st & ~1u;
	__asm__ volatile("mtc0 %0, $12\n sync\n nop\n nop\n nop" ::"r"(off) : "memory");
	return st;
}

static inline void oc_intr_restore(unsigned int st)
{
	__asm__ volatile("mtc0 %0, $12\n sync\n nop\n nop\n nop" ::"r"(st) : "memory");
}

static inline unsigned int oc_num(int mhz)
{
	return (unsigned int)(mhz * OC_DEN / OC_BASE_MHZ);
}

// Wait for the ratio change to finish (bit7). Returns 0 if it does not
static int oc_ratio_wait(void)
{
	for (int i = 0; i < 1000000; i++) {
		__asm__ volatile("nop; nop; nop; nop; nop; nop; nop");
		if (!(HW(0xbc100068) & 0x80))
			return 1;
	}
	return 0;
}

static volatile int g_oc_target;
static volatile unsigned int g_oc_regs[4];

// Kernel mode, diagnostic stages (REI_PSP_OC_DRY): 2 disable interrupts and wait, 3 + rewrite current multiplier, 4 + raise multiplier one step and revert
#ifndef REI_PSP_OC_DRY
#define REI_PSP_OC_DRY 0
#endif
static int oc_kernel_test(void)
{
	const unsigned int st = oc_intr_off();
	oc_settle();
	if (REI_PSP_OC_DRY >= 3) {
		const unsigned int m = HW(0xbc1000fc);
		HW(0xbc1000fc) = m;
		hw_sync();
		oc_settle();
		if (REI_PSP_OC_DRY >= 4) {
			HW(0xbc1000fc) = m + 0x100; // numerator + 1
			hw_sync();
			oc_settle();
			g_oc_regs[0] = HW(0xbc1000fc);
			HW(0xbc1000fc) = m;
			hw_sync();
			oc_settle();
		}
	}
	oc_intr_restore(st);
	return REI_PSP_OC_DRY;
}

// Kernel mode, diagnostic: current PLL/domain register values
static int oc_kernel_read(void)
{
	g_oc_regs[0] = HW(0xbc1000fc);
	g_oc_regs[1] = HW(0xbc100068);
	g_oc_regs[2] = HW(0xbc200000);
	g_oc_regs[3] = HW(0xbc200004);
	return 0;
}

// Kernel mode: up to g_oc_target MHz. Returns the PLL output actually set (MHz) (negative if a ratio step does not finish)
static int oc_kernel(void)
{
	const int target = g_oc_target;
	unsigned int st = oc_intr_off();
	// 1) multiplier to the 333MHz value + upper bits, ratio index to 5, domain ratios slowly up to 1:1 (0x1ff)
	HW(0xbc1000fc) = (HW(0xbc1000fc) & 0xffff0000u) | oc_num(OC_DEFAULT) << 8 | OC_DEN;
	hw_sync();
	HW(0xbc1000fc) = OC_MSB << 16 | (HW(0xbc1000fc) & 0xffffu);
	oc_settle();
	unsigned int idx = HW(0xbc100068) & 0x0f;
	while (idx != OC_RATIO) {
		idx += idx < OC_RATIO ? 1 : -1;
		HW(0xbc100068) = 0x80 | idx;
		hw_sync();
		if (!oc_ratio_wait()) {
			oc_intr_restore(st);
			return -1; // ratio step did not finish
		}
		oc_settle();
	}
	{
		unsigned int c = HW(0xbc200000), b = HW(0xbc200004);
		unsigned int cn = (c >> 16) & 0x1ff, cd = c & 0x1ff, bn = (b >> 16) & 0x1ff, bd = b & 0x1ff;
		while ((cn & cd & bn & bd) != 0x1ff) {
			cn = cn + 18 > 0x1ff ? 0x1ff : cn + 18;
			cd = cd + 18 > 0x1ff ? 0x1ff : cd + 18;
			bn = bn + 18 > 0x1ff ? 0x1ff : bn + 18;
			bd = bd + 18 > 0x1ff ? 0x1ff : bd + 18;
			HW(0xbc200000) = cn << 16 | cd;
			HW(0xbc200004) = bn << 16 | bd;
			oc_settle();
		}
	}
	oc_intr_restore(st);
	// 2) raise in 10MHz steps (briefly re-enabling interrupts each step)
	int mhz = OC_DEFAULT;
	while (mhz + OC_STEP <= target) {
		st = oc_intr_off();
		if (!(HW(0xbc100068) & OC_RATIO)) {
			HW(0xbc100068) = 0x80 | OC_RATIO;
			hw_sync();
			if (!oc_ratio_wait()) {
				oc_intr_restore(st);
				return -2;
			}
		}
		for (unsigned int n = oc_num(mhz); n <= oc_num(mhz + OC_STEP); n++) {
			HW(0xbc1000fc) = OC_MSB << 16 | n << 8 | OC_DEN;
			hw_sync();
		}
		oc_settle();
		oc_intr_restore(st);
		mhz += OC_STEP;
	}
	return (int)(oc_num(mhz) * OC_BASE_MHZ / OC_DEN);
}

// Diagnostic: time (us) of the same loop and of 30 vblanks - did the clock really rise, are system timer/display period unchanged
static void oc_probe(unsigned int *loop_us, unsigned int *vb_us)
{
	unsigned int t0 = sceKernelGetSystemTimeLow();
	for (volatile int i = 0; i < 2000000; i++)
		;
	*loop_us = sceKernelGetSystemTimeLow() - t0;
	sceDisplayWaitVblankStart();
	t0 = sceKernelGetSystemTimeLow();
	for (int i = 0; i < 30; i++)
		sceDisplayWaitVblankStart();
	*vb_us = sceKernelGetSystemTimeLow() - t0;
}

int overclock_set(int mhz)
{
	const int model = kuKernelGetModel(); // 0 = 1000, 1 = 2000, 2 = 3000, 3 = 3000 (09g), 4 = go, then 4000·street
	if (mhz <= OC_DEFAULT)
		return 0;
	if (model < 1 || model > 4) {
		REI_LOG("reirem: overclock: model %d not supported\n", model);
		return -1;
	}
#define OC_STEP_LOG(...) do { REI_LOG(__VA_ARGS__); sceKernelDelayThread(100000); } while (0)
	OC_STEP_LOG("reirem: overclock: model %d\n", model);
	scePowerSetClockFrequency(OC_DEFAULT, OC_DEFAULT, OC_DEFAULT / 2);
	unsigned int l0, v0, l1, v1;
	oc_probe(&l0, &v0);
	OC_STEP_LOG("reirem: overclock: probe %u us, 30 vblanks %u us\n", l0, v0);
	const int lp = meLibLoadPrx(); // kernel bridge kcall.prx (failure is fine if already loaded)
	OC_STEP_LOG("reirem: overclock: kcall bridge %d\n", lp);
	kcall(oc_kernel_read, 0);
	OC_STEP_LOG("reirem: overclock: regs pll mul %08x ratio %08x cpu %08x bus %08x\n", g_oc_regs[0], g_oc_regs[1],
		    g_oc_regs[2], g_oc_regs[3]);
#if REI_PSP_OC_DRY
	if (REI_PSP_OC_DRY >= 2) {
		const int t = kcall(oc_kernel_test, 0);
		OC_STEP_LOG("reirem: overclock: test level %d -> %d, regs pll mul %08x\n", REI_PSP_OC_DRY, t, g_oc_regs[0]);
	}
	return 0; // diagnostic only
#endif
	g_oc_target = mhz;
	const int r = kcall(oc_kernel, 0);
	OC_STEP_LOG("reirem: overclock: PLL step result %d\n", r);
	oc_probe(&l1, &v1);
	REI_LOG("reirem: overclock: model %d, target %d MHz, PLL %d MHz; loop %u -> %u us, 30 vblanks %u -> %u us\n", model,
		mhz, r, l0, l1, v0, v1);
	return r;
}

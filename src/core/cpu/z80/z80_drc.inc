// ReiREM Z80 dynamic recompiler (MIPS: PSP, qemu harness); included by z80.cpp after RM/WM/IN/OUT (docs/DECISIONS.md ADR-024).
//
// Stage 2 (2026-10-15): block = consecutive instructions within one ROM page (256 bytes). Common ops (LD/ALU/INC/DEC/PUSH/POP/16-bit LD/INC/ADD/
// IN/OUT/JR/JP/CALL/RET/DJNZ) are emitted as native code; the rest call the original handler with interpreter-identical state (PRVPC/PC/R/Z80lastop/ICount).
// Registers/flags stay in the Z80 struct, and flag tables (SZP/SZHV_inc/dec/SZHVC_add/sub) are used exactly like the interpreter macros.
// Matching what the interpreter loop does between instructions:
//  - exit after the instruction once ICount <= threshold (max of event cycle and 0) -> dispatcher calls the event (z80_ev_cb)
//  - after instructions that call C (slow memory path/ports/handlers), exit (end_run/map/interrupt/PC) or continue; only C changes interrupt state
//  - blocks end at branch/call/return/RST/HALT/EI/RETN/RETI/repeat ops; the dispatcher does auto-idle detection on backward branches
// Only read-only (ROM) pages are compiled (RAM/encrypted pages use the interpreter). Block key = PC + that page's host pointer.
// Stage 1 (handler calls only) was slower (vigilant 36.7 -> 38.4ms, 2026-10-12) and also skipped runs with auto-idle/events.

#if defined(__mips__) && defined(REI_DRC)
#define Z80_DRC 1
#include <stdint.h>
#ifndef __psp__
#include <sys/mman.h>
#endif

extern int z80_drc_map_dirty;     // z80_intf.cpp: map changed 1, ROM bytes changed 2
UINT8 **ZetDrcMemMap(void);       // z80_intf.cpp: current CPU map (0x000 read, 0x100 write, 0x200 opcode, 0x300 operand)
extern "C" void (*rei_drc_cache_flush)(void *p, int len); // nec_drc.inc (on PSP the frontend provides it)
extern "C" {
int rei_z80_drc_enabled = 0;               // off by default (measured above). Harness REI_Z80DRC_ON
unsigned long long rei_z80_drc_stats[6];   // 0 block runs, 1 blocks compiled, 2 ops compiled, 3 flushes, 4 ops interpreted, 5 RAM/encrypted page interpreted
}
#ifndef __psp__
static int zd_off_mask; // (diag, harness REI_Z80DRC_OFF=mask) op groups not to compile natively; for bisecting a bad group
static struct zd_env { zd_env() { if (getenv("REI_Z80DRC_ON")) rei_z80_drc_enabled = 1; if (getenv("REI_Z80DRC_OFF")) zd_off_mask = (int)strtol(getenv("REI_Z80DRC_OFF"), NULL, 0); } } zd_env_init;
#define ZD_OFF(bit) (zd_off_mask & (bit))
#endif

#ifdef __psp__
#define ZD_OFF(bit) 0
#define ZD_CODE_KB 1024
#else
#define ZD_CODE_KB 512
#endif
#define ZD_HASH 16384        // block table (open addressing)
#define ZD_MAX_INSNS 32      // max ops per block (blocks are per entry point, so long ones fill the cache with overlapping code)

typedef void (*zd_fn)(void);
struct zd_block { UINT32 pc; const UINT8 *host; zd_fn fn; };
static struct zd_block zd_tab[ZD_HASH];
static int zd_used;
static UINT32 *zd_buf, *zd_ptr, *zd_end;
static int zd_overflow;

// --- MIPS32 instruction encoding ---
enum { zZERO = 0, zAT = 1, zV0 = 2, zV1 = 3, zA0 = 4, zA1 = 5, zT0 = 8, zT1 = 9, zT2 = 10, zT3 = 11, zT4 = 12, zT5 = 13,
       zS0 = 16, zS1 = 17, zS2 = 18, zS3 = 19, zS4 = 20, zS5 = 21, zT9 = 25, zSP = 29, zRA = 31 };
static inline void ZE(UINT32 w) { if (zd_ptr < zd_end) *zd_ptr++ = w; else zd_overflow = 1; }
static inline void zR(int rs, int rt, int rd, int sa, int fn) { ZE(((UINT32)rs << 21) | ((UINT32)rt << 16) | ((UINT32)rd << 11) | ((UINT32)sa << 6) | (UINT32)fn); }
static inline void zI(int op, int rs, int rt, int imm) { ZE(((UINT32)op << 26) | ((UINT32)rs << 21) | ((UINT32)rt << 16) | ((UINT32)imm & 0xffff)); }
static inline void z_nop() { ZE(0); }
static inline void z_addiu(int t, int s, int imm) { zI(0x09, s, t, imm); }
static inline void z_ori(int t, int s, int imm) { zI(0x0d, s, t, imm); }
static inline void z_lui(int t, int imm) { zI(0x0f, 0, t, imm); }
static inline void z_lw(int t, int off, int b) { zI(0x23, b, t, off); }
static inline void z_lbu(int t, int off, int b) { zI(0x24, b, t, off); }
static inline void z_lhu(int t, int off, int b) { zI(0x25, b, t, off); }
static inline void z_sw(int t, int off, int b) { zI(0x2b, b, t, off); }
static inline void z_sb(int t, int off, int b) { zI(0x28, b, t, off); }
static inline void z_jr(int s) { zR(s, 0, 0, 0, 0x08); }
static inline void z_jalr(int s) { zR(s, 0, zRA, 0, 0x09); }
static inline UINT32 *z_beq(int s, int t) { UINT32 *p = zd_ptr; zI(0x04, s, t, 0); return p; }
static inline UINT32 *z_bne(int s, int t) { UINT32 *p = zd_ptr; zI(0x05, s, t, 0); return p; }
static inline UINT32 *z_blez(int s) { UINT32 *p = zd_ptr; zI(0x06, s, 0, 0); return p; }
static inline void z_li(int t, UINT32 v)
{
	if ((v >> 16) == 0)
		z_ori(t, zZERO, (int)v);
	else {
		z_lui(t, (int)(v >> 16));
		if (v & 0xffff)
			z_ori(t, t, (int)(v & 0xffff));
	}
}
static inline void z_patch(UINT32 *br, UINT32 *target) // branch br to target
{
	if (!br || zd_overflow || br >= zd_end)
		return;
	*br = (*br & 0xffff0000u) | ((UINT32)(target - br - 1) & 0xffff);
}

#define ZOFS(f) ((int)offsetof(Z80_Regs, f))
#pragma push_macro("EA")
#undef EA
static const int zd_ofs_ea = (int)offsetof(Z80_Regs, EA); // (EA is the Z80.EA macro in z80.cpp)
#pragma pop_macro("EA")
static UINT32 *zd_code0;
static void zd_gen_helpers(void);

static void zd_flush(void)
{
	for (int i = 0; i < ZD_HASH; i++)
		zd_tab[i].fn = NULL, zd_tab[i].host = NULL;
	zd_used = 0;
	zd_ptr = zd_code0;
	rei_z80_drc_stats[3]++;
}

static int zd_init(void)
{
	if (zd_buf)
		return 1;
#ifdef __psp__
	int kb = ZD_CODE_KB;
	if (rei_drc_cache_flush) { // new code can't run without cache functions (smaller if memory is tight)
		zd_buf = (UINT32 *)memalign(64, kb << 10);
		if (!zd_buf) { kb = 256; zd_buf = (UINT32 *)memalign(64, kb << 10); }
	}
#else // qemu harness: executable memory
	const int kb = ZD_CODE_KB;
	void *p = mmap(NULL, kb << 10, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	zd_buf = (p == MAP_FAILED) ? NULL : (UINT32 *)p;
#endif
	if (!zd_buf) {
		rei_z80_drc_enabled = 0; // don't retry (use interpreter)
		return 0;
	}
	zd_end = zd_buf + (kb << 10) / 4;
	zd_gen_helpers();
	zd_flush();
	rei_z80_drc_stats[3] = 0;
	return 1;
}

static void zd_sync(UINT32 *from, UINT32 *to)
{
	if (rei_drc_cache_flush)
		rei_drc_cache_flush(from, (int)((to - from) * 4));
#ifndef __psp__
	else
		__builtin___clear_cache((char *)from, (char *)to);
#endif
}

// instruction length (incl. prefixes). 0 = unknown (block stops there)
static int zd_len(const UINT8 *p, int left)
{
	if (left < 1) return 0;
	const UINT8 op = p[0];
	switch (op) {
	case 0xcb: return 2;
	case 0xed: {
		if (left < 2) return 0;
		const UINT8 o2 = p[1];
		if ((o2 & 0xc7) == 0x43) return 4; // LD (nn),rr / LD rr,(nn)
		return 2;
	}
	case 0xdd: case 0xfd: {
		if (left < 2) return 0;
		const UINT8 o2 = p[1];
		if (o2 == 0xcb) return 4;
		if (o2 == 0xdd || o2 == 0xfd || o2 == 0xed) return 0; // (stacked prefixes: interpreter)
		if (o2 == 0x21 || o2 == 0x22 || o2 == 0x2a) return 4;
		if (o2 == 0x36) return 4;
		if (o2 == 0x34 || o2 == 0x35 || ((o2 & 0xc0) == 0x40 && (o2 & 7) == 6 && o2 != 0x76) ||
		    ((o2 & 0xf8) == 0x70 && o2 != 0x76) || ((o2 & 0xc0) == 0x80 && (o2 & 7) == 6))
			return 3; // (IX+d)
		const int l = zd_len(p + 1, left - 1);
		return l ? 1 + l : 0;
	}
	}
	switch (op) {
	case 0x06: case 0x0e: case 0x16: case 0x1e: case 0x26: case 0x2e: case 0x36: case 0x3e:
	case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
	case 0xc6: case 0xce: case 0xd6: case 0xde: case 0xe6: case 0xee: case 0xf6: case 0xfe:
	case 0xd3: case 0xdb:
		return 2;
	case 0x01: case 0x11: case 0x21: case 0x31: case 0x22: case 0x2a: case 0x32: case 0x3a:
	case 0xc2: case 0xc3: case 0xca: case 0xd2: case 0xda: case 0xe2: case 0xea: case 0xf2: case 0xfa:
	case 0xc4: case 0xcc: case 0xcd: case 0xd4: case 0xdc: case 0xe4: case 0xec: case 0xf4: case 0xfc:
		return 3;
	}
	return 1;
}

// block-ending ops (PC may not be the next address, or interrupt state changes)
static int zd_ends(const UINT8 *p)
{
	UINT8 op = p[0];
	if (op == 0xed) {
		const UINT8 o2 = p[1];
		if ((o2 & 0xc7) == 0x45) return 1;                   // RETN / RETI
		if ((o2 & 0xf4) == 0xb0) return 1;                   // LDIR CPIR INIR OTIR LDDR CPDR INDR OTDR
		return 0;
	}
	if (op == 0xdd || op == 0xfd) {
		op = p[1];
		if (op == 0xcb) return 0;
	}
	if (op == 0x10 || op == 0x18 || (op & 0xe7) == 0x20) return 1; // DJNZ, JR, JR cc
	if ((op & 0xc7) == 0xc0 || op == 0xc9) return 1;               // RET cc, RET
	if ((op & 0xc7) == 0xc2 || op == 0xc3 || op == 0xe9) return 1; // JP cc, JP, JP (HL)
	if ((op & 0xc7) == 0xc4 || op == 0xcd) return 1;               // CALL cc, CALL
	if ((op & 0xc7) == 0xc7) return 1;                             // RST
	if (op == 0x76 || op == 0xfb) return 1;                        // HALT, EI
	return 0;
}

// --- stage 2: instructions to native code ---
// block registers: s0 = &Z80, s1 = ICount, s2 = ops not yet added to R, s3 = exit threshold (max of event cycle and 0:
// exit when ICount <= it), s4 = Z80MemMap, s5 = slow path (C handler) called. Block frame 56 bytes (16(sp) = shared routine ra, 48(sp) = per-op temp)
#define ZD_FRAME 56
static UINT32 *ZH_tail, *ZH_exit, *ZH_rm, *ZH_wm; // shared routines
static INT32 zd_T;                       // set by the dispatcher before calling a block (s3)

static UINT32 zd_rm_slow(UINT32 a) { return RM((UINT16)a); }            // (unmapped side: RM does z80_fx++ and the handler)
static void zd_wm_slow(UINT32 a, UINT32 v) { WM((UINT16)a, (UINT8)v); }
static UINT32 zd_in(UINT32 n) { return IN((INT16)n); }                  // (same type as interpreter op_db)
static void zd_out(UINT32 n, UINT32 v) { OUT((UINT16)n, (UINT8)v); }

static inline void z_sh(int t, int off, int b) { zI(0x29, b, t, off); }
static inline void z_addu(int d, int s, int t) { zR(s, t, d, 0, 0x21); }
static inline void z_subu(int d, int s, int t) { zR(s, t, d, 0, 0x23); }
static inline void z_and(int d, int s, int t) { zR(s, t, d, 0, 0x24); }
static inline void z_or(int d, int s, int t) { zR(s, t, d, 0, 0x25); }
static inline void z_xor(int d, int s, int t) { zR(s, t, d, 0, 0x26); }
static inline void z_slt(int d, int s, int t) { zR(s, t, d, 0, 0x2a); }
static inline void z_sltu(int d, int s, int t) { zR(s, t, d, 0, 0x2b); }
static inline void z_sll(int d, int t, int sa) { zR(0, t, d, sa, 0x00); }
static inline void z_srl(int d, int t, int sa) { zR(0, t, d, sa, 0x02); }
static inline void z_andi(int t, int s, int imm) { zI(0x0c, s, t, imm); }
static inline void z_move(int d, int s) { z_addu(d, s, zZERO); }
static inline void z_j(const UINT32 *t) { ZE((0x02u << 26) | (((UINT32)(uintptr_t)t >> 2) & 0x3ffffff)); }
static inline void z_jal(const UINT32 *t) { ZE((0x03u << 26) | (((UINT32)(uintptr_t)t >> 2) & 0x3ffffff)); }
static inline UINT32 *z_bgez(int s) { UINT32 *p = zd_ptr; zI(0x01, s, 1, 0); return p; }
static inline void z_call_c(const void *fn) { z_li(zT9, (UINT32)(uintptr_t)fn); z_jalr(zT9); z_nop(); }
static inline void z_lbaddr(int t, const void *p) // t = *(UINT8 *)p
{
	const UINT32 a = (UINT32)(uintptr_t)p;
	z_lui(zAT, (int)(((a + 0x8000) >> 16) & 0xffff));
	z_lbu(t, (int)(INT16)(a & 0xffff), zAT);
}

// register slots (Z80 r: 0 B 1 C 2 D 3 E 4 H 5 L 6 (HL) 7 A; pairs low byte first)
static int zd_r8(int r)
{
	switch (r) {
	case 0: return ZOFS(bc) + 1; case 1: return ZOFS(bc);
	case 2: return ZOFS(de) + 1; case 3: return ZOFS(de);
	case 4: return ZOFS(hl) + 1; case 5: return ZOFS(hl);
	default: return ZOFS(af) + 1;
	}
}
#define ZF_OFS (ZOFS(af))
static int zd_r16(int k) { switch (k) { case 0: return ZOFS(bc); case 1: return ZOFS(de); case 2: return ZOFS(hl); default: return ZOFS(sp); } }
static int zd_r16af(int k) { switch (k) { case 0: return ZOFS(bc); case 1: return ZOFS(de); case 2: return ZOFS(hl); default: return ZOFS(af); } }

static void zd_gen_helpers(void)
{
	zd_ptr = zd_buf;
	// tail: store ICount/R and return (PRVPC/PC/Z80lastop already stored by the caller)
	ZH_tail = zd_ptr;
	z_sw(zS1, ZOFS(ICount), zS0);
	z_lbu(zT0, ZOFS(r), zS0);
	z_addu(zT0, zT0, zS2);
	z_sb(zT0, ZOFS(r), zS0);
	z_lw(zRA, 44, zSP);
	z_lw(zS0, 40, zSP); z_lw(zS1, 36, zSP); z_lw(zS2, 32, zSP); z_lw(zS3, 28, zSP); z_lw(zS4, 24, zSP); z_lw(zS5, 20, zSP);
	z_jr(zRA);
	z_addiu(zSP, zSP, ZD_FRAME);
	// stub exit: t0 = PRVPC << 16 | PC, t1 = op -> store and go to tail
	ZH_exit = zd_ptr;
	z_srl(zT2, zT0, 16); z_sw(zT2, ZOFS(prvpc), zS0);
	z_sh(zT0, ZOFS(pc), zS0);
	z_li(zAT, (UINT32)(uintptr_t)&Z80lastop); z_sw(zT1, 0, zAT);
	z_j(ZH_tail);
	z_nop();
	// read: a0 = address (16-bit) -> v0. Uses at/v1. Unmapped: C (store ICount, reload after return, s5 = 1)
	ZH_rm = zd_ptr;
	z_srl(zAT, zA0, 8); z_sll(zAT, zAT, 2); z_addu(zAT, zAT, zS4); z_lw(zAT, 0, zAT);
	UINT32 *slow = z_beq(zAT, zZERO);
	z_andi(zV1, zA0, 0xff);
	z_addu(zAT, zAT, zV1);
	z_jr(zRA);
	z_lbu(zV0, 0, zAT);
	z_patch(slow, zd_ptr);
	z_sw(zRA, 16, zSP);
	z_sw(zS1, ZOFS(ICount), zS0);
	z_ori(zS5, zZERO, 1);
	z_call_c((const void *)zd_rm_slow);
	z_lw(zS1, ZOFS(ICount), zS0);
	z_lw(zRA, 16, zSP);
	z_jr(zRA);
	z_nop();
	// write: a0 = address, a1 = value (byte). If mapped, z80_fx += (changed) (same as interpreter WM). Uses at/v1/a0
	ZH_wm = zd_ptr;
	z_srl(zAT, zA0, 8); z_sll(zAT, zAT, 2); z_addu(zAT, zAT, zS4); z_lw(zAT, 0x400, zAT);
	slow = z_beq(zAT, zZERO);
	z_andi(zV1, zA0, 0xff);
	z_addu(zAT, zAT, zV1);
	z_lbu(zV1, 0, zAT);
	z_sb(zA1, 0, zAT);
	z_xor(zV1, zV1, zA1);
	z_andi(zV1, zV1, 0xff);
	z_sltu(zV1, zZERO, zV1);
	{
		const UINT32 fx = (UINT32)(uintptr_t)&z80_fx;
		z_lui(zA0, (int)(((fx + 0x8000) >> 16) & 0xffff));
		z_lw(zAT, (int)(INT16)(fx & 0xffff), zA0);
		z_addu(zAT, zAT, zV1);
		z_sw(zAT, (int)(INT16)(fx & 0xffff), zA0);
	}
	z_jr(zRA);
	z_nop();
	z_patch(slow, zd_ptr);
	z_sw(zRA, 16, zSP);
	z_sw(zS1, ZOFS(ICount), zS0);
	z_ori(zS5, zZERO, 1);
	z_call_c((const void *)zd_wm_slow);
	z_lw(zS1, ZOFS(ICount), zS0);
	z_lw(zRA, 16, zSP);
	z_jr(zRA);
	z_nop();
	zd_code0 = zd_ptr;
	zd_sync(zd_buf, zd_ptr);
}

static inline void zd_rm(void) { z_jal(ZH_rm); z_nop(); }   // a0 → v0
static inline void zd_wm(void) { z_jal(ZH_wm); z_nop(); }   // a0, a1

// operand byte -> t1 (src r: 0..7, 6 = (HL) read). Returns 1 if memory was read
static int zd_src8(int r)
{
	if (r == 6) {
		z_lhu(zA0, ZOFS(hl), zS0);
		zd_rm();
		z_move(zT1, zV0);
		return 1;
	}
	z_lbu(zT1, zd_r8(r), zS0);
	return 0;
}

// A op t1 (ALU 0 ADD 1 ADC 2 SUB 3 SBC 4 AND 5 XOR 6 OR 7 CP); same values as interpreter macros (BIG_FLAGS_ARRAY)
static void zd_alu(int k)
{
	const int A_ = zd_r8(7);
	z_lbu(zT0, A_, zS0);
	switch (k) {
	case 0: case 1: case 2: case 3: {
		const int sub = k >= 2, carry = k & 1;
		if (carry) { z_lbu(zT5, ZF_OFS, zS0); z_andi(zT5, zT5, 1); }
		if (sub) z_subu(zT2, zT0, zT1); else z_addu(zT2, zT0, zT1);
		if (carry) { if (sub) z_subu(zT2, zT2, zT5); else z_addu(zT2, zT2, zT5); }
		z_andi(zT2, zT2, 0xff);
		z_sll(zT3, zT0, 8);
		z_or(zT3, zT3, zT2);
		if (carry) { z_sll(zT5, zT5, 16); z_or(zT3, zT3, zT5); }
		z_li(zAT, (UINT32)(uintptr_t)(sub ? &SZHVC_sub : &SZHVC_add));
		z_lw(zAT, 0, zAT);
		z_addu(zAT, zAT, zT3);
		z_lbu(zT4, 0, zAT);
		z_sb(zT4, ZF_OFS, zS0);
		z_sb(zT2, A_, zS0);
		break;
	}
	case 4: case 5: case 6:
		if (k == 4) z_and(zT0, zT0, zT1); else if (k == 5) z_xor(zT0, zT0, zT1); else z_or(zT0, zT0, zT1);
		z_sb(zT0, A_, zS0);
		z_li(zAT, (UINT32)(uintptr_t)SZP);
		z_addu(zAT, zAT, zT0);
		z_lbu(zT4, 0, zAT);
		if (k == 4) z_ori(zT4, zT4, HF);
		z_sb(zT4, ZF_OFS, zS0);
		break;
	case 7: // CP: F = (SZHVC_sub[A<<8 | res] & ~(YF|XF)) | (val & (YF|XF))
		z_subu(zT2, zT0, zT1);
		z_andi(zT2, zT2, 0xff);
		z_sll(zT3, zT0, 8);
		z_or(zT3, zT3, zT2);
		z_li(zAT, (UINT32)(uintptr_t)&SZHVC_sub);
		z_lw(zAT, 0, zAT);
		z_addu(zAT, zAT, zT3);
		z_lbu(zT4, 0, zAT);
		z_andi(zT4, zT4, 0xff & ~(YF | XF));
		z_andi(zT5, zT1, YF | XF);
		z_or(zT4, zT4, zT5);
		z_sb(zT4, ZF_OFS, zS0);
		break;
	}
}

// condition (Z80 cc 0..7: NZ Z NC C PO PE P M) -> t0 = 0 means false. Returns: 1 = "true if t0 != 0", 0 = "true if t0 == 0"
static int zd_cond(int cc)
{
	static const UINT8 mask[4] = { ZF, CF, PF, SF };
	z_lbu(zT0, ZF_OFS, zS0);
	z_andi(zT0, zT0, mask[cc >> 1]);
	return cc & 1;
}

// exit (block-end stub): store PRVPC = a, PC = next, Z80lastop = op and go to tail
struct zd_exit { UINT32 *br; UINT32 a, next; UINT8 op; };

// native block-ending op: store PRVPC/Z80lastop, set PC/WZ, go to tail. 1 = emitted
static int zd_emit_branch(UINT32 a, const UINT8 *p, UINT8 op)
{
	const int pcofs = ZOFS(pc);
	int kind = -1, cond = -1;
	if (op == 0x18 || op == 0x10 || (op & 0xe7) == 0x20) kind = 0;          // JR, DJNZ, JR cc
	else if (op == 0xc3 || (op & 0xc7) == 0xc2) kind = 1;                    // JP, JP cc
	else if (op == 0xcd) kind = 2;                                           // CALL
	else if (op == 0xc9 || (op & 0xc7) == 0xc0) kind = 3;                    // RET, RET cc
	else if (op == 0xe9) kind = 4;                                           // JP (HL)
	if (kind < 0 || ZD_OFF(2048))
		return 0;
	// PRVPC, Z80lastop
	z_li(zT0, a); z_sw(zT0, ZOFS(prvpc), zS0);
	z_li(zT0, op); z_li(zAT, (UINT32)(uintptr_t)&Z80lastop); z_sw(zT0, 0, zAT);
	UINT32 *skip = NULL, *done = NULL;
	if (kind == 0) {
		const UINT32 nt = (a + 2) & 0xffff, tg = (nt + (INT8)p[1]) & 0xffff;
		if (op == 0x10) { // DJNZ: B--, if B != 0
			z_lbu(zT0, zd_r8(0), zS0); z_addiu(zT0, zT0, -1); z_andi(zT0, zT0, 0xff); z_sb(zT0, zd_r8(0), zS0);
			skip = z_beq(zT0, zZERO);
		} else if (op != 0x18) {
			const int t = zd_cond((op >> 3) & 3);
			skip = t ? z_beq(zT0, zZERO) : z_bne(zT0, zZERO);
		}
		if (skip) z_nop();
		if (op != 0x18) z_addiu(zS1, zS1, -(int)cc[Z80_TABLE_ex][op]); // (conditional branch taken)
		z_li(zT0, tg); z_sh(zT0, pcofs, zS0); z_sh(zT0, ZOFS(wz), zS0);
		if (skip) {
			done = z_beq(zZERO, zZERO); z_nop();
			z_patch(skip, zd_ptr);
			z_li(zT0, nt); z_sh(zT0, pcofs, zS0);
		}
	} else if (kind == 1) {
		const UINT32 nn = p[1] | (p[2] << 8);
		if (op != 0xc3) {
			const int t = zd_cond((op >> 3) & 7);
			skip = t ? z_beq(zT0, zZERO) : z_bne(zT0, zZERO);
			z_nop();
		}
		z_li(zT0, nn); z_sw(zT0, pcofs, zS0); z_sh(zT0, ZOFS(wz), zS0);      // (PCD = nn)
		if (skip) {
			done = z_beq(zZERO, zZERO); z_nop();
			z_patch(skip, zd_ptr);
			z_li(zT0, nn); z_sh(zT0, ZOFS(wz), zS0);
			z_li(zT0, (a + 3) & 0xffff); z_sh(zT0, pcofs, zS0);
		}
	} else if (kind == 2) { // CALL: EA = nn, WZ = EA, PUSH(pc = a+3), PCD = EA
		const UINT32 nn = p[1] | (p[2] << 8), ret = (a + 3) & 0xffff;
		z_li(zT0, nn); z_sw(zT0, zd_ofs_ea, zS0); z_sh(zT0, ZOFS(wz), zS0);
		z_li(zT0, ret); z_sh(zT0, pcofs, zS0);
		for (int h = 1; h >= 0; h--) { // SP--; WM(SP, pc.h) / SP--; WM(SP, pc.l)
			z_lhu(zA0, ZOFS(sp), zS0); z_addiu(zA0, zA0, -1); z_andi(zA0, zA0, 0xffff); z_sh(zA0, ZOFS(sp), zS0);
			z_li(zA1, h ? (ret >> 8) : (ret & 0xff));
			zd_wm();
		}
		z_li(zT0, nn); z_sw(zT0, pcofs, zS0);
	} else if (kind == 3) { // RET (cc): POP(pc), WZ = PCD (RET) / PC (RET cc)
		if (op != 0xc9) {
			const int t = zd_cond((op >> 3) & 7);
			skip = t ? z_beq(zT0, zZERO) : z_bne(zT0, zZERO);
			z_nop();
			z_addiu(zS1, zS1, -(int)cc[Z80_TABLE_ex][op]);
		}
		z_lhu(zA0, ZOFS(sp), zS0); zd_rm(); z_sb(zV0, pcofs, zS0);
		z_lhu(zA0, ZOFS(sp), zS0); z_addiu(zA0, zA0, 1); z_andi(zA0, zA0, 0xffff); zd_rm(); z_sb(zV0, pcofs + 1, zS0);
		z_lhu(zT0, ZOFS(sp), zS0); z_addiu(zT0, zT0, 2); z_sh(zT0, ZOFS(sp), zS0);
		z_lw(zT0, pcofs, zS0); z_sh(zT0, ZOFS(wz), zS0);
		if (skip) {
			done = z_beq(zZERO, zZERO); z_nop();
			z_patch(skip, zd_ptr);
			z_li(zT0, (a + 1) & 0xffff); z_sh(zT0, pcofs, zS0);
		}
	} else { // JP (HL): PC = HL
		z_lhu(zT0, ZOFS(hl), zS0); z_sh(zT0, pcofs, zS0);
	}
	if (done) z_patch(done, zd_ptr);
	z_j(ZH_tail);
	z_nop();
	return 1;
}

static UINT8 zd_lastop; // (Z80lastop stored by native ops: second byte for prefixed ops, like interpreter ROP)

// DD/FD prefix (IX/IY): interpreter op_dd/op_fd = R++, EXEC(xy, ROP()); cycles from xy table, Z80lastop = second byte.
// EAX/EAY: EA = (UINT16)(I? + d), WZ = EA. 0 = unsupported
static int zd_emit_xy(const UINT8 *p, UINT8 pre)
{
	const UINT8 o = p[1];
	const int xr = pre == 0xdd ? ZOFS(ix) : ZOFS(iy);
	if (ZD_OFF(512)) return 0;
	int kind = 0;
	// check support first (before subtracting cycles/R)
	const int ld_r_m = (o & 0xc7) == 0x46 && o != 0x76;               // LD r,(I?+d)
	const int ld_m_r = (o & 0xf8) == 0x70 && o != 0x76;               // LD (I?+d),r
	const int alu_m = (o & 0xc7) == 0x86;                             // ALU A,(I?+d)
	const int ok = o == 0x21 || o == 0x23 || o == 0x2b || (o & 0xcf) == 0x09 || o == 0xe5 || o == 0xe1 || o == 0x36 ||
		ld_r_m || ld_m_r || alu_m || o == 0x34 || o == 0x35;
	if (!ok) return 0;
	z_addiu(zS1, zS1, -(int)cc[Z80_TABLE_xy][o]);
	z_addiu(zS2, zS2, 1);
	zd_lastop = o;
	// (I?+d) address -> a0, stored to EA/WZ
	#define ZD_EA() do { z_lhu(zA0, xr, zS0); z_addiu(zA0, zA0, (INT8)p[2]); z_andi(zA0, zA0, 0xffff); 		z_sw(zA0, zd_ofs_ea, zS0); z_sh(zA0, ZOFS(wz), zS0); } while (0)
	if (o == 0x21) { z_li(zT0, p[2] | (p[3] << 8)); z_sh(zT0, xr, zS0); kind = 1; }
	else if (o == 0x23 || o == 0x2b) { z_lhu(zT0, xr, zS0); z_addiu(zT0, zT0, o == 0x23 ? 1 : -1); z_sh(zT0, xr, zS0); kind = 1; }
	else if ((o & 0xcf) == 0x09) { // ADD I?,rr (ADD16, .d as is; 29 = I? itself)
		const int k = o >> 4, so = k == 0 ? ZOFS(bc) : k == 1 ? ZOFS(de) : k == 2 ? xr : ZOFS(sp);
		z_lw(zT0, xr, zS0);
		z_lw(zT1, so, zS0);
		z_addu(zT2, zT0, zT1);
		z_addiu(zT3, zT0, 1); z_sh(zT3, ZOFS(wz), zS0);
		z_xor(zT3, zT0, zT2); z_xor(zT3, zT3, zT1); z_srl(zT3, zT3, 8); z_andi(zT3, zT3, HF);
		z_srl(zT4, zT2, 16); z_andi(zT4, zT4, CF); z_or(zT3, zT3, zT4);
		z_srl(zT4, zT2, 8); z_andi(zT4, zT4, YF | XF); z_or(zT3, zT3, zT4);
		z_lbu(zT4, ZF_OFS, zS0); z_andi(zT4, zT4, SF | ZF | VF); z_or(zT3, zT3, zT4);
		z_sb(zT3, ZF_OFS, zS0);
		z_sh(zT2, xr, zS0);
		kind = 1;
	} else if (o == 0xe5) {
		for (int h = 1; h >= 0; h--) {
			z_lhu(zA0, ZOFS(sp), zS0); z_addiu(zA0, zA0, -1); z_andi(zA0, zA0, 0xffff); z_sh(zA0, ZOFS(sp), zS0);
			z_lbu(zA1, xr + h, zS0);
			zd_wm();
		}
		kind = 2;
	} else if (o == 0xe1) {
		z_lhu(zA0, ZOFS(sp), zS0); zd_rm(); z_sb(zV0, xr, zS0);
		z_lhu(zA0, ZOFS(sp), zS0); z_addiu(zA0, zA0, 1); z_andi(zA0, zA0, 0xffff); zd_rm(); z_sb(zV0, xr + 1, zS0);
		z_lhu(zT0, ZOFS(sp), zS0); z_addiu(zT0, zT0, 2); z_sh(zT0, ZOFS(sp), zS0);
		kind = 2;
	} else if (o == 0x36) { ZD_EA(); z_li(zA1, p[3]); zd_wm(); kind = 2; }
	else if (ld_r_m) { ZD_EA(); zd_rm(); z_sb(zV0, zd_r8((o >> 3) & 7), zS0); kind = 2; }
	else if (ld_m_r) { ZD_EA(); z_lbu(zA1, zd_r8(o & 7), zS0); zd_wm(); kind = 2; }
	else if (alu_m) { ZD_EA(); zd_rm(); z_move(zT1, zV0); zd_alu((o >> 3) & 7); kind = 2; }
	else { // INC/DEC (I?+d): EA, v = RM(EA), F = (F & CF) | SZHV_inc/dec[v±1], WM(EA, v±1)
		ZD_EA();
		z_sw(zA0, 48, zSP); // (address into a free frame slot, for write after read)
		zd_rm();
		z_addiu(zT0, zV0, o == 0x34 ? 1 : -1); z_andi(zT0, zT0, 0xff);
		z_lbu(zT1, ZF_OFS, zS0); z_andi(zT1, zT1, CF);
		z_li(zAT, (UINT32)(uintptr_t)(o == 0x35 ? SZHV_dec : SZHV_inc)); z_addu(zAT, zAT, zT0); z_lbu(zT2, 0, zAT);
		z_or(zT1, zT1, zT2); z_sb(zT1, ZF_OFS, zS0);
		z_lw(zA0, 48, zSP); z_move(zA1, zT0); zd_wm();
		kind = 2;
	}
	#undef ZD_EA
	return kind;
}

// in-block native op (not a branch). 0 = unsupported (handler), 1 = pure, 2 = memory (exit after if slow path), 3 = always exit after (ports)
static int zd_emit_insn(UINT32 a, const UINT8 *p, UINT8 op)
{
	zd_lastop = op;
	if (op == 0xdd || op == 0xfd)
		return zd_emit_xy(p, op);
	if (op >= 0x40 && op < 0x80 && op != 0x76 && !ZD_OFF(1)) { // LD r,r'
		const int d = (op >> 3) & 7, s = op & 7;
		if (d == 6) { // LD (HL),r
			z_lhu(zA0, ZOFS(hl), zS0); z_lbu(zA1, zd_r8(s), zS0); zd_wm();
			return 2;
		}
		const int m = zd_src8(s);
		z_sb(zT1, zd_r8(d), zS0);
		return m ? 2 : 1;
	}
	if (op >= 0x80 && op < 0xc0 && !ZD_OFF(2) && !ZD_OFF(0x10000 << ((op >> 3) & 7))) { // ALU A,r
		const int m = zd_src8(op & 7);
		zd_alu((op >> 3) & 7);
		return m ? 2 : 1;
	}
	if ((op & 0xc7) == 0xc6 && !ZD_OFF(2) && !ZD_OFF(0x10000 << ((op >> 3) & 7))) { // ALU A,n
		z_li(zT1, p[1]);
		zd_alu((op >> 3) & 7);
		return 1;
	}
	if ((op & 0xc7) == 0x06 && op != 0x36 && !ZD_OFF(4)) { z_li(zT0, p[1]); z_sb(zT0, zd_r8((op >> 3) & 7), zS0); return 1; } // LD r,n
	if (op == 0x36 && !ZD_OFF(4)) { z_lhu(zA0, ZOFS(hl), zS0); z_li(zA1, p[1]); zd_wm(); return 2; }          // LD (HL),n
	if ((op & 0xcf) == 0x01 && !ZD_OFF(8)) { z_li(zT0, p[1] | (p[2] << 8)); z_sh(zT0, zd_r16(op >> 4), zS0); return 1; } // LD rr,nn
	if (((op & 0xcf) == 0x03 || (op & 0xcf) == 0x0b) && !ZD_OFF(16)) { // INC rr / DEC rr
		const int o = zd_r16(op >> 4);
		z_lhu(zT0, o, zS0); z_addiu(zT0, zT0, (op & 8) ? -1 : 1); z_sh(zT0, o, zS0);
		return 1;
	}
	if (((op & 0xc7) == 0x04 || (op & 0xc7) == 0x05) && op != 0x34 && op != 0x35 && !ZD_OFF(32) && !ZD_OFF((op & 1) ? 0x2000000 : 0x1000000)) { // INC r / DEC r
		const int o = zd_r8((op >> 3) & 7), dec = op & 1;
		z_lbu(zT0, o, zS0); z_addiu(zT0, zT0, dec ? -1 : 1); z_andi(zT0, zT0, 0xff); z_sb(zT0, o, zS0);
		z_lbu(zT1, ZF_OFS, zS0); z_andi(zT1, zT1, CF);
		z_li(zAT, (UINT32)(uintptr_t)(dec ? SZHV_dec : SZHV_inc)); z_addu(zAT, zAT, zT0); z_lbu(zT2, 0, zAT);
		z_or(zT1, zT1, zT2); z_sb(zT1, ZF_OFS, zS0);
		return 1;
	}
	if ((op & 0xcf) == 0x09 && !ZD_OFF(64)) { // ADD HL,rr (ADD16, .d as is)
		z_lw(zT0, ZOFS(hl), zS0);
		z_lw(zT1, zd_r16(op >> 4), zS0);
		z_addu(zT2, zT0, zT1);
		z_addiu(zT3, zT0, 1); z_sh(zT3, ZOFS(wz), zS0);
		z_xor(zT3, zT0, zT2); z_xor(zT3, zT3, zT1); z_srl(zT3, zT3, 8); z_andi(zT3, zT3, HF);
		z_srl(zT4, zT2, 16); z_andi(zT4, zT4, CF); z_or(zT3, zT3, zT4);
		z_srl(zT4, zT2, 8); z_andi(zT4, zT4, YF | XF); z_or(zT3, zT3, zT4);
		z_lbu(zT4, ZF_OFS, zS0); z_andi(zT4, zT4, SF | ZF | VF); z_or(zT3, zT3, zT4);
		z_sb(zT3, ZF_OFS, zS0);
		z_sh(zT2, ZOFS(hl), zS0);
		return 1;
	}
	if ((op & 0xcf) == 0xc5 && !ZD_OFF(128)) { // PUSH rr: SP--, WM(SP, h), SP--, WM(SP, l)
		const int o = zd_r16af((op >> 4) & 3);
		for (int h = 1; h >= 0; h--) {
			z_lhu(zA0, ZOFS(sp), zS0); z_addiu(zA0, zA0, -1); z_andi(zA0, zA0, 0xffff); z_sh(zA0, ZOFS(sp), zS0);
			z_lbu(zA1, o + h, zS0);
			zd_wm();
		}
		return 2;
	}
	if ((op & 0xcf) == 0xc1 && !ZD_OFF(128)) { // POP rr: l = RM(SP), h = RM(SP+1), SP += 2
		const int o = zd_r16af((op >> 4) & 3);
		z_lhu(zA0, ZOFS(sp), zS0); zd_rm(); z_sb(zV0, o, zS0);
		z_lhu(zA0, ZOFS(sp), zS0); z_addiu(zA0, zA0, 1); z_andi(zA0, zA0, 0xffff); zd_rm(); z_sb(zV0, o + 1, zS0);
		z_lhu(zT0, ZOFS(sp), zS0); z_addiu(zT0, zT0, 2); z_sh(zT0, ZOFS(sp), zS0);
		return 2;
	}
	if (ZD_OFF(256)) return 0;
	switch (op) {
	case 0x00: return 1; // NOP
	case 0x32: { // LD (nn),A: EA = nn, WM(EA, A), WZ_L = nn+1, WZ_H = A
		const UINT32 nn = p[1] | (p[2] << 8);
		z_li(zT0, nn); z_sw(zT0, zd_ofs_ea, zS0);
		z_li(zA0, nn); z_lbu(zA1, zd_r8(7), zS0); zd_wm();
		z_li(zT0, (nn + 1) & 0xff); z_sb(zT0, ZOFS(wz), zS0);
		z_lbu(zT0, zd_r8(7), zS0); z_sb(zT0, ZOFS(wz) + 1, zS0);
		return 2;
	}
	case 0x3a: { // LD A,(nn): EA = nn, A = RM(EA), WZ = EA + 1
		const UINT32 nn = p[1] | (p[2] << 8);
		z_li(zT0, nn); z_sw(zT0, zd_ofs_ea, zS0);
		z_li(zA0, nn); zd_rm(); z_sb(zV0, zd_r8(7), zS0);
		z_li(zT0, (nn + 1) & 0xffff); z_sh(zT0, ZOFS(wz), zS0);
		return 2;
	}
	case 0x2a: case 0x22: { // LD HL,(nn) / LD (nn),HL: EA = nn, two bytes, WZ = EA + 1
		const UINT32 nn = p[1] | (p[2] << 8);
		z_li(zT0, nn); z_sw(zT0, zd_ofs_ea, zS0);
		for (int h = 0; h < 2; h++) {
			z_li(zA0, (nn + h) & 0xffff);
			if (op == 0x2a) { zd_rm(); z_sb(zV0, ZOFS(hl) + h, zS0); }
			else { z_lbu(zA1, ZOFS(hl) + h, zS0); zd_wm(); }
		}
		z_li(zT0, (nn + 1) & 0xffff); z_sh(zT0, ZOFS(wz), zS0);
		return 2;
	}
	case 0xeb: // EX DE,HL (whole pairs)
		z_lw(zT0, ZOFS(de), zS0); z_lw(zT1, ZOFS(hl), zS0); z_sw(zT1, ZOFS(de), zS0); z_sw(zT0, ZOFS(hl), zS0);
		return 1;
	case 0x08: // EX AF,AF'
		z_lw(zT0, ZOFS(af), zS0); z_lw(zT1, ZOFS(af2), zS0); z_sw(zT1, ZOFS(af), zS0); z_sw(zT0, ZOFS(af2), zS0);
		return 1;
	case 0xd9: // EXX
		z_lw(zT0, ZOFS(bc), zS0); z_lw(zT1, ZOFS(bc2), zS0); z_sw(zT1, ZOFS(bc), zS0); z_sw(zT0, ZOFS(bc2), zS0);
		z_lw(zT0, ZOFS(de), zS0); z_lw(zT1, ZOFS(de2), zS0); z_sw(zT1, ZOFS(de), zS0); z_sw(zT0, ZOFS(de2), zS0);
		z_lw(zT0, ZOFS(hl), zS0); z_lw(zT1, ZOFS(hl2), zS0); z_sw(zT1, ZOFS(hl), zS0); z_sw(zT0, ZOFS(hl2), zS0);
		return 1;
	case 0xf3: // DI
		z_sb(zZERO, ZOFS(iff1), zS0); z_sb(zZERO, ZOFS(iff2), zS0);
		return 1;
	case 0xdb: { // IN A,(n): n = arg | A << 8, WZ = n + 1, A = IN(n)
		z_lbu(zT0, zd_r8(7), zS0); z_sll(zT0, zT0, 8); z_ori(zT0, zT0, p[1]);
		z_addiu(zT1, zT0, 1); z_sh(zT1, ZOFS(wz), zS0);
		z_move(zA0, zT0);
		z_sw(zS1, ZOFS(ICount), zS0);
		z_call_c((const void *)zd_in);
		z_lw(zS1, ZOFS(ICount), zS0);
		z_sb(zV0, zd_r8(7), zS0);
		return 3;
	}
	case 0xd3: { // OUT (n),A: OUT(n, A), WZ_L = n+1, WZ_H = A
		z_lbu(zT0, zd_r8(7), zS0); z_sll(zA0, zT0, 8); z_ori(zA0, zA0, p[1]); z_move(zA1, zT0);
		z_sw(zS1, ZOFS(ICount), zS0);
		z_call_c((const void *)zd_out);
		z_lw(zS1, ZOFS(ICount), zS0);
		z_li(zT0, (p[1] + 1) & 0xff); z_sb(zT0, ZOFS(wz), zS0);
		z_lbu(zT0, zd_r8(7), zS0); z_sb(zT0, ZOFS(wz) + 1, zS0);
		return 3;
	}
	}
	return 0;
}

// compile a block: NULL on failure (that op uses the interpreter)
static zd_fn zd_compile(UINT32 pc, const UINT8 *host)
{
	if (zd_end - zd_ptr < 8192 || zd_used >= ZD_HASH * 3 / 4)
		zd_flush();
	UINT32 *start = zd_ptr;
	zd_overflow = 0;
	struct zd_exit ex[ZD_MAX_INSNS * 2 + 8];
	int nex = 0;
	// no block if the first op can't be native (a handler-call-only block is no better than the interpreter)
	{
		const int off = pc & 0xff;
		const int len = zd_len(host + off, 0x100 - off);
		if (!len || off + len > 0x100)
			return NULL;
		const UINT8 op = host[off];
		const int native = (op >= 0x40 && op < 0xc0 && op != 0x76) || (op & 0xc7) == 0xc6 || (op & 0xc7) == 0x06 ||
			(op & 0xcf) == 0x01 || (op & 0xc7) == 0x04 || (op & 0xc7) == 0x05 || (op & 0xc7) == 0x03 ||
			(op & 0xcf) == 0x09 || (op & 0xcf) == 0xc5 || (op & 0xcf) == 0xc1 || op == 0x18 || op == 0x10 ||
			(op & 0xe7) == 0x20 || op == 0xc3 || (op & 0xc7) == 0xc2 || op == 0xcd || op == 0xc9 || (op & 0xc7) == 0xc0 ||
			op == 0x32 || op == 0x3a || op == 0x2a || op == 0x22 || op == 0xeb || op == 0xd9 || op == 0xdb || op == 0xd3;
		if (!native)
			return NULL;
	}
	// prologue: frame, s0 = &Z80, s1 = ICount, s2 = 0, s3 = threshold, s4 = map, s5 = 0
	z_addiu(zSP, zSP, -ZD_FRAME);
	z_sw(zRA, 44, zSP);
	z_sw(zS0, 40, zSP); z_sw(zS1, 36, zSP); z_sw(zS2, 32, zSP); z_sw(zS3, 28, zSP); z_sw(zS4, 24, zSP); z_sw(zS5, 20, zSP);
	z_li(zS0, (UINT32)(uintptr_t)&Z80);
	z_lw(zS1, ZOFS(ICount), zS0);
	z_move(zS2, zZERO);
	z_li(zAT, (UINT32)(uintptr_t)&zd_T); z_lw(zS3, 0, zAT);
	z_li(zAT, (UINT32)(uintptr_t)&Z80MemMap); z_lw(zS4, 0, zAT);
	z_move(zS5, zZERO);
	int n = 0;
	UINT32 a = pc;
	int open = 0; // (code continues after the previous op: ending the block here needs an exit after that op)
	UINT32 pa = 0, pn = 0;
	UINT8 pop = 0;
	for (;;) {
		const int off = a & 0xff;
		const int len = zd_len(host + off, 0x100 - off);
		if (!len || off + len > 0x100 || (a >> 8) != (pc >> 8)) {
			if (open) { ex[nex].br = z_beq(zZERO, zZERO); z_nop(); ex[nex].a = pa; ex[nex].next = pn; ex[nex++].op = pop; }
			break;
		}
		const UINT8 *p = host + off;
		const UINT8 op = p[0];
		const UINT32 next = (a + len) & 0xffff;
		const int ends = zd_ends(p);
		// interpreter loop: R++, CC(op)
		z_addiu(zS1, zS1, -(int)cc[Z80_TABLE_op][op]);
		z_addiu(zS2, zS2, 1);
		n++;
		if (ends) {
			if (zd_emit_branch(a, p, op))
				break;
		}
		int kind = ends ? 0 : zd_emit_insn(a, p, op);
		if (kind == 0) { // handler: call with interpreter-identical state (PRVPC/PC/Z80lastop/ICount/R), reload on return
			z_li(zT0, a); z_sw(zT0, ZOFS(prvpc), zS0);
			z_li(zT0, (a + 1) & 0xffff); z_sh(zT0, ZOFS(pc), zS0);
			z_li(zT0, op); z_li(zAT, (UINT32)(uintptr_t)&Z80lastop); z_sw(zT0, 0, zAT);
			z_sw(zS1, ZOFS(ICount), zS0);
			z_lbu(zT0, ZOFS(r), zS0); z_addu(zT0, zT0, zS2); z_sb(zT0, ZOFS(r), zS0); z_move(zS2, zZERO);
			z_call_c((const void *)Z80op[op]);
			z_lw(zS1, ZOFS(ICount), zS0);
			if (ends || n >= ZD_MAX_INSNS) { z_j(ZH_tail); z_nop(); break; }
			// exit reasons: end_run, map changed, pending interrupt, PC not next (state stored by the handler -> straight to tail)
			z_lw(zT0, ZOFS(end_run), zS0); UINT32 *e1 = z_bne(zT0, zZERO); z_nop();
			z_li(zAT, (UINT32)(uintptr_t)&z80_drc_map_dirty); z_lw(zT0, 0, zAT); UINT32 *e2 = z_bne(zT0, zZERO); z_nop();
			z_lbu(zT0, ZOFS(irq_state), zS0); UINT32 *ok = z_beq(zT0, zZERO); z_nop();
			z_lbu(zT0, ZOFS(iff1), zS0); UINT32 *e3 = z_bne(zT0, zZERO); z_nop();
			z_patch(ok, zd_ptr);
			z_lhu(zT0, ZOFS(pc), zS0); z_li(zT1, next); UINT32 *e4 = z_bne(zT0, zT1); z_nop();
			z_slt(zT0, zS3, zS1); UINT32 *e5 = z_beq(zT0, zZERO); z_nop();
			UINT32 *cont = z_beq(zZERO, zZERO); z_nop();
			z_patch(e1, zd_ptr); z_patch(e2, zd_ptr); z_patch(e3, zd_ptr); z_patch(e4, zd_ptr); z_patch(e5, zd_ptr);
			z_j(ZH_tail); z_nop();
			z_patch(cont, zd_ptr);
			open = 1; pa = a; pn = next; pop = op;
			a = next;
			continue;
		}
		const UINT8 lop = zd_lastop;
		if (kind == 2) { ex[nex].br = z_bne(zS5, zZERO); z_nop(); ex[nex].a = a; ex[nex].next = next; ex[nex++].op = lop; }
		const int last = kind == 3 || n >= ZD_MAX_INSNS || (next >> 8) != (pc >> 8) || nex >= ZD_MAX_INSNS * 2;
		if (last) { ex[nex].br = z_beq(zZERO, zZERO); z_nop(); ex[nex].a = a; ex[nex].next = next; ex[nex++].op = lop; break; }
		z_slt(zT0, zS3, zS1);
		ex[nex].br = z_beq(zT0, zZERO); z_nop(); ex[nex].a = a; ex[nex].next = next; ex[nex++].op = lop;
		open = 1; pa = a; pn = next; pop = lop;
		a = next;
	}
	// stub: store PRVPC/PC/Z80lastop and go to tail
	for (int i = 0; i < nex; i++) {
		z_patch(ex[i].br, zd_ptr);
		z_li(zT0, (ex[i].a << 16) | ex[i].next);
		z_j(ZH_exit);
		z_ori(zT1, zZERO, ex[i].op);
	}
	if (zd_overflow) {
		zd_flush();
		return NULL;
	}
	zd_sync(start, zd_ptr);
	rei_z80_drc_stats[1]++;
	rei_z80_drc_stats[2] += n;
	return (zd_fn)(void *)start;
}

static inline UINT32 zd_hash(UINT32 pc, const UINT8 *host) { return (pc ^ (pc >> 7) ^ ((UINT32)(uintptr_t)host >> 8)) & (ZD_HASH - 1); }

// block for current PC (compile if missing). NULL if it should run in the interpreter
static zd_fn zd_lookup(void)
{
	if (z80_drc_map_dirty) {
		if (z80_drc_map_dirty == 2)
			zd_flush();
		z80_drc_map_dirty = 0;
	}
	const UINT32 pc = PCD & 0xffff;
	UINT8 **map = ZetDrcMemMap();
	const UINT8 *host = map[0x200 | (pc >> 8)];
	if (!host || map[0x100 | (pc >> 8)] == host || map[0x300 | (pc >> 8)] != host) {
		rei_z80_drc_stats[5]++;
		return NULL;
	}
	UINT32 h = zd_hash(pc, host);
	for (;;) {
		struct zd_block *b = &zd_tab[h];
		if (!b->host)
			break;
		if (b->pc == pc && b->host == host)
			return b->fn;
		h = (h + 1) & (ZD_HASH - 1);
	}
	zd_fn fn = zd_compile(pc, host);
	if (!fn)
		return NULL;
	h = zd_hash(pc, host); // (compiling may have flushed the table)
	while (zd_tab[h].host)
		h = (h + 1) & (ZD_HASH - 1);
	zd_tab[h].pc = pc;
	zd_tab[h].host = host;
	zd_tab[h].fn = fn;
	zd_used++;
	return fn;
}

#endif // __mips__ && REI_DRC

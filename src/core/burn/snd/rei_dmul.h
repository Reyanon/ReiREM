// ReiREM: (INT32)((double)x * v): integer x double constant with results identical to IEEE-754 double, without floating point.
//
// The PSP (Allegrex) FPU is single-precision only, so double multiply/convert go to soft-float (__floatsidf/__muldf3/__fixdfsi).
// Sound chip output volume (AY8910, MSM5205 ...) called these per sample/channel, costing several ms of a PSP other-board frame.
// Split v into a 53-bit integer m and 2^-k, multiply |x|*m as integers, then only check whether rounding to 53 bits (ties to even) like IEEE
// carries into the integer part (double multiply rounds once, int conversion truncates toward 0). Out of range (|x| >= 2^20, very large or
// small v) uses the original double expression. tools/dmul_test.c compares exhaustively against real double.
#ifndef REI_DMUL_H
#define REI_DMUL_H

#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	double v;     // original value (when out of range)
	uint64_t m;   // v = m × 2^-k, m ∈ [2^52, 2^53)
	int k;        // fast path only for 32..63
	int fast;     // use fast path
	int zero;     // v == 0
	int inited;   // (rei_dmul_set)
} rei_dmul_t;

static inline void rei_dmul_init(rei_dmul_t *c, double v)
{
	c->inited = 1;
	c->v = v;
	c->zero = (v == 0.0);
	c->fast = 0;
	c->m = 0;
	c->k = 0;
	if (c->zero || !(v > 0.0) || v != v)
		return; // (negative/NaN use the original expression)
	int e;
	const double fr = frexp(v, &e); // v = fr × 2^e, fr ∈ [0.5, 1)
	const uint64_t m = (uint64_t)ldexp(fr, 53); // exact (53 bits)
	const int k = 53 - e;
	if (k < 32 || k > 63 || m < (1ULL << 52) || m >= (1ULL << 53))
		return;
	c->m = m;
	c->k = k;
	c->fast = 1;
}

// re-prepare only when the value changes (bitwise compare: double compare is soft-float too). Zero-fill c initially
static inline void rei_dmul_set(rei_dmul_t *c, double v)
{
	uint64_t a, b;
	memcpy(&a, &v, 8);
	memcpy(&b, &c->v, 8);
	if (!c->inited || a != b)
		rei_dmul_init(c, v);
}

static inline int rei_dmul_clz64(uint64_t v) { return v >> 32 ? __builtin_clz((uint32_t)(v >> 32)) : 32 + __builtin_clz((uint32_t)v); }

static inline int32_t rei_dmul(int32_t x, const rei_dmul_t *c)
{
	if (c->zero || x == 0)
		return 0;
	const uint32_t ax = x < 0 ? (uint32_t)-x : (uint32_t)x;
	if (!c->fast || ax >= (1u << 20))
		return (int32_t)((double)x * c->v);
	// P = ax * m = hi * 2^32 + p0 (up to 73 bits, hi < 2^41)
	const uint32_t ml = (uint32_t)c->m, mh = (uint32_t)(c->m >> 32);
	const uint64_t lo = (uint64_t)ax * ml;
	const uint64_t hi = (uint64_t)ax * mh + (lo >> 32);
	const uint32_t p0 = (uint32_t)lo;
	const int k = c->k;
	uint32_t q = (uint32_t)(hi >> (k - 32)); // integer part (P >> k)
	const int L = hi ? 96 - rei_dmul_clz64(hi) : 32 - __builtin_clz(p0); // bit count of P
	const int d = L - 53; // bits dropped by rounding (<= 20)
	if (d > 0) {
		// fraction f = P mod 2^k (k <= 63 bits)
		const uint64_t f = ((hi & ((1ULL << (k - 32)) - 1)) << 32) | p0;
		const uint64_t rem = f & ((1ULL << d) - 1), half = 1ULL << (d - 1);
		const int up = rem > half || (rem == half && ((f >> d) & 1));
		if (up && (f >> d) == (1ULL << (k - d)) - 1)
			q++; // rounding carries into the integer part
	}
	return x < 0 ? -(int32_t)q : (int32_t)q;
}

// (INT32)((double)D + v * (double)c): sound chip DC-removal filter (AY8910 0.997, MSM5205 0.998) with identical results.
// For v in [0.5, 1) (k = 53), |c| < 2^16, |D| < 2^20, in 128-bit integers (two 64-bit, 2^-64 units): t = round53(c*v),
// S = D + t, round53(S), truncate toward 0. If c != 0 then |t| >= 0.5, so t's last bit fits within 2^-64. Otherwise the original expression.
typedef struct { uint64_t hi, lo; } rei_u128;
static inline int rei_u128_bits(rei_u128 a) { return a.hi ? 128 - rei_dmul_clz64(a.hi) : (a.lo ? 64 - rei_dmul_clz64(a.lo) : 0); }

static inline int32_t rei_dmul_add(int32_t D, int32_t c, const rei_dmul_t *cv)
{
	const uint32_t ac = c < 0 ? (uint32_t)-c : (uint32_t)c, aD = D < 0 ? (uint32_t)-D : (uint32_t)D;
	if (!cv->fast || cv->k != 53 || ac >= (1u << 16) || aD >= (1u << 20))
		return (int32_t)((double)D + cv->v * (double)c);
	if (c == 0)
		return D; // (t = 0)
	// round P = ac * m (<= 69 bits) to 53 bits -> t = P * 2^-53
	const uint64_t lo = (uint64_t)ac * (uint32_t)cv->m;
	uint64_t phi = (uint64_t)ac * (uint32_t)(cv->m >> 32) + (lo >> 32); // P = phi × 2^32 + (uint32)lo
	uint64_t plo = (uint32_t)lo;
	{
		const int L = phi ? 96 - rei_dmul_clz64(phi) : 32 - __builtin_clz((uint32_t)plo);
		const int d = L - 53; // (≤ 16)
		if (d > 0) {
			const uint64_t P = (phi << 32) | plo; // (if d > 0 then L <= 69: round in the low 64 bits, high part from phi)
			const uint64_t rem = P & ((1ULL << d) - 1), half = 1ULL << (d - 1);
			const int up = rem > half || (rem == half && ((P >> d) & 1));
			// P -= rem (+ 2^d): split into low 32 bits and high phi
			uint64_t low = plo - (rem & 0xffffffffu); // (rem < 2^16: within plo)
			plo = low;
			if (up) {
				plo += 1ULL << d;
				if (plo >> 32) { phi += plo >> 32; plo &= 0xffffffffu; }
			}
		}
	}
	// t * 2^64 = P * 2^11 (128-bit), D * 2^64
	rei_u128 T, S;
	T.hi = phi >> 21;                                   // high 64 bits of (phi * 2^32 + plo) * 2^11
	T.lo = (phi << 43) | (plo << 11);
	const int neg_t = c < 0, neg_D = D < 0;
	int neg;
	if (neg_t == neg_D || D == 0) { // add magnitudes
		S.lo = T.lo;
		S.hi = T.hi + aD;
		neg = (D == 0) ? neg_t : neg_D;
	} else if (T.hi >= aD) { // |t| >= |D| (T.hi = integer part of |t|)
		S.lo = T.lo;
		S.hi = T.hi - aD;
		neg = neg_t;
	} else { // |D| > |t|: S = D·2^64 - T
		S.lo = 0 - T.lo;
		S.hi = aD - T.hi - (T.lo != 0);
		neg = neg_D;
	}
	// round S to 53 bits (ties to even)
	const int L = rei_u128_bits(S);
	const int d = L - 53;
	if (d > 0) { // (L <= 85 -> d <= 32, within the low 64 bits)
		const uint64_t rem = S.lo & ((1ULL << d) - 1), half = 1ULL << (d - 1);
		const int up = rem > half || (rem == half && ((S.lo >> d) & 1));
		S.lo -= rem;
		if (up) {
			const uint64_t before = S.lo;
			S.lo += 1ULL << d;
			if (S.lo < before) S.hi++;
		}
	}
	const uint32_t q = (uint32_t)S.hi; // truncate toward 0 = integer part (magnitude)
	return neg ? -(int32_t)q : (int32_t)q;
}

#endif

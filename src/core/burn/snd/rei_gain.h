// ReiREM PSP port: exact integer form of FBNeo's per-sample "(INT32)(sample * double_gain)".
// The PSP's FPU has no double precision, so every such multiply is a soft-float call (int->double,
// multiply, double->int, a few hundred cycles), thousands of times per frame (GA20, YM2151 mix).
// For gain == k/5.0 (as a double) and |x * k| < 2^24, (INT32)(x * gain) == (x * k) / 5 (C truncation)
// - checked exhaustively for x in [-2^24, 2^24] with k in {1,2,3,4,5,6,8,10} (k = 7 differs only
// above 2^24). Sample values here stay below 2^20. Other gains keep the double multiply.
#ifndef REI_GAIN_H
#define REI_GAIN_H

static inline int rei_gain_k(double gain)
{
	static const int ok[] = { 1, 2, 3, 4, 5, 6, 8, 10 };
	for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
		if (gain == ok[i] / 5.0)
			return ok[i];
	return -1;
}

#endif

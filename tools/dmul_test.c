// rei_dmul.h test: compare (int32)((double)x * v) with rei_dmul(x, v) for all x in [-2^20, 2^20] and several v.
// Build: cc -O2 -I src/core/burn/snd tools/dmul_test.c -lm -o /tmp/dmul_test && /tmp/dmul_test
#include <stdio.h>
#include <stdlib.h>
#include "rei_dmul.h"

static int check(double v)
{
	rei_dmul_t c;
	rei_dmul_init(&c, v);
	int bad = 0;
	for (int32_t x = -(1 << 20); x <= (1 << 20); x++) {
		const int32_t want = (int32_t)((double)x * v), got = rei_dmul(x, &c);
		if (want != got && ++bad <= 3)
			printf("  v=%.17g x=%d want %d got %d\n", v, x, want, got);
	}
	return bad;
}

// rei_dmul_add: (int32)((double)D + v * (double)c), c over all INT16, D wide (edges + strides + random)
static int check_add(double v, int seed)
{
	rei_dmul_t cv;
	rei_dmul_init(&cv, v);
	int bad = 0;
	srand(seed);
	for (int32_t c = -32768; c <= 32767; c++) {
		for (int j = 0; j < 96; j++) {
			int32_t D;
			if (j < 32) D = j - 16;                                   // near 0
			else if (j < 48) D = -c + (j - 40);                       // where it cancels t
			else if (j < 64) D = (int32_t)((rand() % 140001) - 70000); // random
			else D = ((j & 1) ? 1 : -1) * ((1 << ((j - 64) / 2 + 2)) + (rand() % 7) - 3); // near powers of 2
			const int32_t want = (int32_t)((double)D + v * (double)c), got = rei_dmul_add(D, c, &cv);
			if (want != got && ++bad <= 3)
				printf("  add v=%.17g D=%d c=%d want %d got %d\n", v, D, c, want, got);
		}
	}
	return bad;
}

int main(int argc, char **argv)
{
	{
		int bad = check_add(0.997, 7) + check_add(0.998, 8) + check_add(0.995, 12) + check_add(0.5, 9) + check_add(0.75, 10) +
			check_add(0.9999, 11);
		printf("dmul_add: %d mismatches\n", bad);
		if (bad) return 1;
	}
	// volumes used by drivers and their 1/3 (AY8910 pan), DC filter 0.997, other values
	static const double vs[] = { 0.07, 0.10, 0.11, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40, 0.45, 0.50, 0.60, 0.65, 0.70, 0.75, 0.80,
		0.85, 0.90, 1.00, 1.20, 1.50, 2.00, 0.997, 0.07 / 3, 0.15 / 3, 0.25 / 3, 0.5 / 3, 1.0 / 3, 0.33, 0.66, 3.0, 4.0 };
	int bad = 0, n = 0;
	for (unsigned i = 0; i < sizeof(vs) / sizeof(vs[0]); i++, n++)
		bad += check(vs[i]);
	srand(argc > 1 ? atoi(argv[1]) : 1);
	for (int i = 0; i < 40; i++, n++) { // random values (0..4)
		const double v = ldexp((double)(((uint64_t)rand() << 31) ^ (uint64_t)rand()), -60) * 4.0;
		bad += check(v);
	}
	printf("dmul_test: %d values, %d mismatches\n", n, bad);
	return bad != 0;
}

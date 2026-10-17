// 64-bit atomics (psp/docs/RULES.md 4-1). Allegrex is 32-bit, so GCC emits __atomic_*_8 as library calls,
// which pspdev lacks. Wrap them in a 32-bit spinlock for the core 64-bit shared counters (d_m92.cpp rei_main_done etc.).
// Main-CPU threads only: the ME shares neither cache nor atomics, so it never uses this path.
#include <stdint.h>

static volatile int g_lock;

static inline void lock(void)
{
	while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
		while (__atomic_load_n(&g_lock, __ATOMIC_RELAXED))
			;
}

static inline void unlock(void)
{
	__atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

uint64_t __atomic_load_8(const volatile void *p, int order)
{
	(void)order;
	lock();
	uint64_t v = *(const volatile uint64_t *)p;
	unlock();
	return v;
}

void __atomic_store_8(volatile void *p, uint64_t v, int order)
{
	(void)order;
	lock();
	*(volatile uint64_t *)p = v;
	unlock();
}

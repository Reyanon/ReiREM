// PSP overclock (PSP-2000/3000/go, psp/src/overclock.c). Call before starting the ME.
#ifndef REI_PSP_OVERCLOCK_H
#define REI_PSP_OVERCLOCK_H

// If mhz <= 333, does nothing and returns 0. -1 on unsupported models. Otherwise the PLL output set (MHz)
int overclock_set(int mhz);

#endif

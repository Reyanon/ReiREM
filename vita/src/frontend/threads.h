#ifndef REI_FRONTEND_THREADS_H
#define REI_FRONTEND_THREADS_H

// Core assignment (docs/RULES.md ch. 3)
//   CPU0: main (V33, video, input, vsync) - main() thread
//   CPU1: sound (V35, YM2151, GA20 -> audio ring) - thread in this file
//   CPU2: render (added in M3-c)

// Pin the main thread to CPU0
void rei_threads_pin_main(void);

// Start the sound thread on CPU1. Call after rei_core_init(threaded=1)
int rei_threads_start_sound(void);

// Other boards: YM2151 synthesis worker on CPU1 (only if the core supports it, 0 = started)
int rei_threads_start_ym(void);

// Stop and join the sound/YM threads (includes rei_core_stop)
void rei_threads_stop(void);

#endif

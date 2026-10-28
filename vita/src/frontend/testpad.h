// Test input (log builds only): ux0:data/reirem/keys holds a script of button presses and touches that the app plays
// as if they came from the pad and the touch screen (the device tests drive the menus with it; the vitacompanion
// "press" command does not reach the app's pad reads). Tokens, separated by spaces or new lines:
//   circle | down*5            a tap (4 frames down, 8 up), optionally repeated
//   select+start               keys pressed together
//   hold:right:90              held for that many frames
//   wait:60                    nothing for that many frames
//   touch:480:272:10           a touch at x, y for that many frames
//   drag:100:100:300:200:30    a touch moving from x0, y0 to x1, y1 over that many frames
// The file is removed when it is read; a new one can be sent at any time.
#ifndef REI_FRONTEND_TESTPAD_H
#define REI_FRONTEND_TESTPAD_H

#include <stdint.h>

#ifdef REI_ENABLE_LOG
// once per frame from the loop that reads the pad: the buttons the script presses now
uint32_t rei_testpad_step(void);
// the touch the script makes now (1, x, y in screen pixels) or 0
int rei_testpad_touch(int *x, int *y);
#else
static inline uint32_t rei_testpad_step(void) { return 0; }
static inline int rei_testpad_touch(int *x, int *y) { (void)x; (void)y; return 0; }
#endif

#endif

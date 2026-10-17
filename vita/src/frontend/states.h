#ifndef REI_FRONTEND_STATES_H
#define REI_FRONTEND_STATES_H

// Save state slots: 0-9, quicksave (REI_STATE_QUICK).
// File: ux0:data/reirem/<romset>_<slot>.sav (per set)

#define REI_STATE_SLOTS 10
#define REI_STATE_QUICK (-1)

void rei_state_path(int slot, char *buf, int len);
int rei_state_exists(int slot);
// "slot 3" / "quick"
const char *rei_state_slot_name(int slot);

#endif

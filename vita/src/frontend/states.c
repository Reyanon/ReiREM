#include "frontend/states.h"

#include <stdio.h>

#include "rei_core.h"

#include "frontend/settings.h"

#define STATE_DIR REI_DATA_DIR

void rei_state_path(int slot, char *buf, int len)
{
	if (slot == REI_STATE_QUICK)
		snprintf(buf, len, STATE_DIR "%s_quick.sav", rei_core_set_name());
	else
		snprintf(buf, len, STATE_DIR "%s_%d.sav", rei_core_set_name(), slot);
}

int rei_state_exists(int slot)
{
	char path[128];
	rei_state_path(slot, path, sizeof(path));
	return rei_core_state_exists(path);
}

const char *rei_state_slot_name(int slot)
{
	static char buf[16];
	if (slot == REI_STATE_QUICK)
		return "quick";
	snprintf(buf, sizeof(buf), "slot %d", slot);
	return buf;
}

#include "m6800_intf.h"
#include "msm5205.h"
#include "ay8910.h"

void IremSoundWrite(UINT8 d);
// (ReiREM) batched execution: collect writes during main CPU steps (Begin/Slice/End) and apply them before sound slices (Apply)
void IremSoundBatchBegin();
void IremSoundBatchSlice(INT32 i);
INT32 IremSoundBatchEnd();
void IremSoundBatchApply(INT32 i);
extern UINT8 IremSlaveMSM5205VClckReset;
void IremSoundClockSlave();

INT32 IremSoundReset();
void IremSoundInit(UINT8 *pZ80ROM, INT32 nType, INT32 nZ80Clock);
void IremSoundInit(UINT8 *pZ80ROM, INT32 nType, INT32 nZ80Clock, void (*ay8910cb)(UINT8));
INT32 IremSoundExit();
INT32 IremSoundScan(INT32 nAction, INT32 *pnMin);

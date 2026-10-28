// ReiREM kernel helper, user side (vita/kmod/rei_kctrl.c)
#ifndef REI_KCTRL_H
#define REI_KCTRL_H
unsigned int reiKCtrlPsState(void); // bits 0-14 single PS presses, 15-29 double presses, 30 held
int reiKCtrlGoHome(void);           // press PS for the system once (after the app lets go of its lock)
int reiKCtrlInstallOnExit(void);    // once the app has exited: ux0:data/reirem/update/files -> the app folder
#endif

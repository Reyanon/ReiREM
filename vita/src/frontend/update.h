// Update check and install from the GitHub releases of Reyanon/reirem (ported from ReSys12 / ReST-V)
#ifndef REI_FRONTEND_UPDATE_H
#define REI_FRONTEND_UPDATE_H

// start the check on its own thread (network, GitHub API); nothing when the measuring tools started the app
void rei_update_check_start(void);
// 1 once a release newer than this build (and not skipped) is known
int rei_update_ready(void);
const char *rei_update_version(void); // its tag, e.g. "v0.1.2"
const char *rei_update_notes(void);   // the notes of every newer release, newest first (plain text, one line per '\n')
void rei_update_skip(void);           // do not offer this version again
// download, unpack and hand over to the kernel helper (does not return then). progress(stage, percent) is called
// while it runs (percent -1: unknown, -2: the stage text alone). Returns a message on failure
const char *rei_update_install(void (*progress)(const char *stage, int percent));
// called just before the install exits the app (the PS button back to the system)
extern void (*rei_update_before_exit)(void);

#endif

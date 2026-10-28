#!/bin/bash
# Device check before installing a play build (PSP HANDOFF "regression log", added 2026-10-13 so failures do not recur).
#
# Installs a test build matching the log-free play config (build-psp-playtest: REI_PSP_PLAYER=ON, REI_LOG=OFF, REI_PSP_TESTGAME=ON)
# at the install location (ms0:/PSP/GAME/REIREM/EBOOT.PBP) and, per game, checks
#   XMB → launch installed build (host0:/rei_game, skipping the list) → play 40 s → screen moving (two captures) →
#   HOME exit (host0:/rei_home = HOME > exit game callback) → back to XMB within 30 s
# If any check fails, the play build is not installed. On success the real play build (build-psp-play) is installed.
#
#   bash psp/tools/play_check.sh [game set ...]   (default: nbbatman rtype vigilant kengo airass)
# Requires: PSP at XMB with USB (PSPLink plugin) connected, usbhostfs_pc serving build-psp as host0:.
set -u
export PSPDEV=${PSPDEV:-$HOME/pspdev}; export PATH=$PSPDEV/bin:$PATH
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; D="$ROOT/build-psp"; cd "$ROOT"
GAMES=${*:-nbbatman rtype vigilant kengo airass}
# usbhostfs_pc (host0:=build-psp, host1:=repo). Restarted if it drops when USB re-enumerates
uh_ok() {
	pgrep -x usbhostfs_pc >/dev/null && timeout 8 pspsh -e "ls host0:/" >/dev/null 2>&1 && return 0
	pkill -x usbhostfs_pc; sleep 1
	(cd "$D" && nohup usbhostfs_pc "$D" "$ROOT" > /tmp/usbhostfs_pc.log 2>&1 &)
	echo "$D|$ROOT" > /tmp/usbhostfs_pc.root
	for i in $(seq 1 20); do grep -q "Connected to device" /tmp/usbhostfs_pc.log 2>/dev/null && break; sleep 0.5; done
	sleep 2
}
psh() { timeout "${T:-20}" pspsh -e "$*" 2>/dev/null | tr -d '\r'; }
in_xmb() { uh_ok; psh modlist | grep -q "Name: vsh_module"; }
in_dev() { uh_ok; psh modlist | grep -q "Name: reiremdev$"; }
# Is the screen moving: any of 5 captures 2 s apart differ (briefly static screens like the title pass)
moving() {
	local prev="" n=0
	for k in 1 2 3 4 5; do
		rm -f "$D/shot.bmp"; T=30 psh "scrshot host0:/shot.bmp" >/dev/null
		[ -f "$D/shot.bmp" ] || return 2
		n=$((n+1)); local h; h=$(md5sum < "$D/shot.bmp")
		[ -n "$prev" ] && [ "$h" != "$prev" ] && { rm -f "$D/shot.bmp"; return 0; }
		prev=$h; sleep 2
	done
	return 1
}
# Hung device diagnosis (regression log rule 6): state and wait object of each of our threads, where main is stuck
diag() {
	echo "--- diag $(date +%T)"
	psh thlist | grep -a "Name:" | grep -avi "Name: Sce" | while read -r _ uid _ _ name; do
		echo "$name $(psh "thinfo $uid" | grep -a "Status\|WaitId" | tr '\n' ' ')"
		w=$(psh "thinfo $uid" | grep -ao "WaitId: 0x[0-9A-Fa-f]*" | awk '{print $2}')
		[ -n "$w" ] && [ "$w" != "0x00000000" ] && echo "   waiting: $(psh "uidinfo $w" | head -1)"
		[ "$name" = "user_main" ] && echo "   $(psh "thctx $uid" | grep -a "Real EPC")"
	done
	psh exprint | head -12 # (threads killed by exceptions: EPC/address shown here if KILLED or sleeping)
	echo "--- app folder me_trace tail"
	rm -f "$D/pc_trace.txt"; T=60 psh "cp ms0:/PSP/GAME/REIREM/me_trace.txt host0:/pc_trace.txt" >/dev/null; tail -8 "$D/pc_trace.txt" 2>/dev/null
}
# After diagnosis, PSPLink reset back to XMB (hung device recovery, allowed 2026-10-17). RESET_ON_FAIL=0 leaves it as is
fail() {
	echo "FAIL: $*"; diag; rm -f "$D/rei_game" "$D/rei_home" "$D/rei_select" "$D/rei_ysplit"
	# (No reset on failures before a game is launched — build errors, or the device in use (not XMB/REIREM DEV): a reset once killed
	#  an installed build mid-play, 2026-10-17)
	if [ "${RESET_ON_FAIL:-1}" = 1 ] && [ "${STARTED:-0}" = 1 ]; then
		psh reset >/dev/null
		ok=0; for i in $(seq 1 30); do sleep 3; in_xmb && { ok=1; break; }; done
		[ $ok = 1 ] && echo "reset: back to XMB" || echo "reset: did not return to XMB (power cycle needed)"
	fi
	exit 1
}

psp-cmake -S psp -B build-psp-playtest -DCMAKE_BUILD_TYPE=Release -DREI_PSP_PLAYER=ON -DREI_LOG=OFF -DREI_PSP_TESTGAME=ON \
	-DREI_PSP_S2M_DELAY=224 >/dev/null || fail "test build configure"
cmake --build build-psp-playtest -j 2>&1 | grep -aE " error|FAILED" && fail "test build"
cmake --build build-psp-play -j 2>&1 | grep -aE " error|FAILED" && fail "play build"

install_eboot() { # $1 = EBOOT.PBP. Large file copies only work inside REIREM DEV
	if ! in_dev; then
		in_xmb || fail "PSP is neither at XMB nor in REIREM DEV (if unresponsive, hold power to turn off)"
		psh "ldstart host0:/devstub/nbblaunch.prx" >/dev/null
		for i in $(seq 1 40); do sleep 1; in_dev && break; done
		in_dev || fail "REIREM DEV did not start"
	fi
	cp "$1" "$D/chk_EBOOT.PBP"
	psh "rm ms0:/PSP/GAME/REIREM/EBOOT.PBP" >/dev/null # (overwriting a file copied from a PC can fail silently: read-only attribute)
	T=120 psh "cp host0:/chk_EBOOT.PBP ms0:/PSP/GAME/REIREM/EBOOT.PBP" >/dev/null
	rm -f "$D/check.pbp"; T=120 psh "cp ms0:/PSP/GAME/REIREM/EBOOT.PBP host0:/check.pbp" >/dev/null
	for i in $(seq 1 12); do cmp -s "$D/check.pbp" "$D/chk_EBOOT.PBP" && break; sleep 5; done # (the read-back can finish after pspsh returns)
	cmp -s "$D/check.pbp" "$D/chk_EBOOT.PBP" || fail "install verify ($1)"
	rm -f "$D/check.pbp" "$D/chk_EBOOT.PBP"
}

install_eboot build-psp-playtest/EBOOT.PBP
echo "installing test build"
# All test features are now on by default (play-tested 2026-10-17) — remove old enable files from the app folder. Disable files are no<name>
for f in meym meysplit z80drc m107ge m107mesnd m92drc35 m72ge; do psh "rm ms0:/PSP/GAME/REIREM/$f" >/dev/null; done
psh "ls ms0:/PSP/GAME/REIREM" | grep -E " (meym|meysplit|z80drc|m107ge|m107mesnd|m92drc35|m72ge)$" && fail "old enable files remain"
echo "old enable files cleaned"
# YSPLIT=1: the test build switches the YM2151 split at frames 900/1200/1500/1800 (exercises the switch path even in the demo)
[ "${YSPLIT:-0}" = 1 ] && : > "$D/rei_ysplit"
# SWITCH=1: move between games the same way as options "Select game" (host0:/rei_select: the game ends, the app goes back to its
#           game list in the same process and the list starts host0:/rei_game — no relaunch). Only the last game exits via HOME.
#           The test build appends the free memory at each return to the list to host0:/rei_mem.txt (shown at the end: must not shrink)
n=0; set -- $GAMES; last=$#
rm -f "$D/rei_mem.txt"
STARTED=1 # (from here on, failures reset back to XMB)
for g in $GAMES; do
	n=$((n+1))
	echo "$(date +%T) $g start ($n/$last)"
	echo "$g" > "$D/rei_game"
	if [ "${SWITCH:-0}" = 1 ] && [ $n -gt 1 ]; then
		touch "$D/rei_select" # (the previous game picks it up, ends and the list in the same process starts this one)
		for i in $(seq 1 15); do sleep 2; [ -f "$D/rei_select" ] || break; done
		[ -f "$D/rei_select" ] && fail "$g: previous game did not take Select game (hung)"
	else
		psh "ldstart host0:/devstub/nbblaunch_play.prx" >/dev/null
	fi
	sleep ${PLAY_S:-40} # (PLAY_S: play seconds per game — 60+ for things that switch after 1200 frames, like the split)
	uh_ok; moving; m=$?
	[ $m = 2 ] && fail "$g: capture failed (PSPLink not responding — hung)"
	[ $m = 1 ] && fail "$g: screen unchanged for 10 s (hung)"
	if [ "${SWITCH:-0}" = 1 ] && [ $n -lt $last ]; then
		echo "PASS $g (start, run)"
		continue
	fi
	echo "$(date +%T) $g running, HOME exit"
	rm -f "$D/rei_game"; touch "$D/rei_home"
	ok=0; for i in $(seq 1 15); do sleep 2; in_xmb && { ok=1; break; }; done
	rm -f "$D/rei_home"
	[ $ok = 1 ] || fail "$g: did not return to XMB within 30 s after HOME exit"
	echo "PASS $g (start, run, HOME exit)"
	sleep 3
done
rm -f "$D/rei_ysplit"
[ "${SWITCH:-0}" = 1 ] && { echo "free memory at each return to the list:"; cat "$D/rei_mem.txt" 2>/dev/null; }
install_eboot build-psp-play/EBOOT.PBP
echo "installing play build (all passed)"
echo "PSP is in REIREM DEV (exit with HOME and launch ReiREM). Leaving the installed build running blocks the next check"

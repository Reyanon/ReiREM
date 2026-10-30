#!/usr/bin/env bash
# Run directly on the PSP via PSPLink (no Memory Stick copy or EBOOT install). psp/docs/RULES.md ch. 7.
#   bash psp/tools/run.sh [prx path, default build-psp/reirem.plain.prx]
#   bash psp/tools/run.sh --install-dev   # copy the empty dev game REIREM DEV to PSP/GAME/REIREMDEV (once; go uses ef0:)
# (build-psp/reirem.prx is encrypted for EBOOT use and does not load via PSPLink)
#
# Setup: PSPLink (plugin) + USB on the PSP; on Windows attach it to WSL with psp/tools/attach_psp.ps1 -Auto,
#       one-time WSL setup: sudo bash psp/tools/setup_psplink_wsl.sh. If the PSP is at the XMB, run.sh launches REIREM DEV.
#
# PSPLink is a plugin (ms0:/seplugins/psplink.ini), so it is resident in both the XMB and games.
# - Loading a module at the XMB fights the XMB for GE/memory and freezes the screen → load only in game mode.
# - Game mode is held by REIREM DEV (psp/devstub), which uses almost no memory. If a previously loaded reirem remains, ask it to
#   stop (host0:/rei_stop) so it unloads itself. kill leaks the heap (next load gets 0x800200D9); reset leaves to the XMB even in game mode
#   and the XMB then shows "failed to start", so neither is used. modexec did not work on this CFW either.
# - usbhostfs_pc exports the PRX folder as host0: and the ROM folder (REI_ROM_DIR, default repo root) as host1:.
# - PSP printf (stdout/stderr) arrives on usbhostfs_pc ports 10002/10003. Saved to $OUT; with PSP_FOLLOW=1 (default)
#   it keeps showing after launch (Ctrl+C to stop; the PSP app keeps running).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export PSPDEV="${PSPDEV:-$HOME/pspdev}"
export PATH="$PSPDEV/bin:$PATH"
LOG="${PSPLINK_LOG:-/tmp/usbhostfs_pc.log}"
OUT="${PSP_STDOUT_LOG:-/tmp/psp_stdout.log}"
FOLLOW="${PSP_FOLLOW:-1}"
ROMDIR="$(cd "${REI_ROM_DIR:-$ROOT}" && pwd)"

INSTALL_DEV=0
if [ "${1:-}" = "--install-dev" ]; then
	INSTALL_DEV=1
	shift
fi
PRX="${1:-$ROOT/build-psp/reirem.plain.prx}"
if [ ! -f "$PRX" ]; then
	echo "run: $PRX not found. Run bash tools/build.sh psp first" >&2
	exit 2
fi
# host0: is always build-psp (launcher and stop file live there). A PRX elsewhere is copied there and loaded
DIR="$ROOT/build-psp"
NAME="$(basename "$PRX")"
if [ "$(cd "$(dirname "$PRX")" && pwd)" != "$DIR" ]; then
	cp "$PRX" "$DIR/$NAME"
fi

psh() { timeout "${PSH_TIMEOUT:-10}" pspsh -e "$*" 2>/dev/null | tr -d '\r'; }
die_frozen() {
	echo "run: PSP not responding (frozen?). Hold the power switch for 10 s to force it off, power on, then run REIREM DEV" >&2
	exit 1
}
connects() { grep -c "Connected to device" "$LOG" 2>/dev/null || echo 0; }

# restart usbhostfs_pc if it is serving a different folder
if pgrep -x usbhostfs_pc >/dev/null; then
	if [ "$(cat /tmp/usbhostfs_pc.root 2>/dev/null)" != "$DIR|$ROMDIR" ]; then
		pkill -x usbhostfs_pc || true
		sleep 1
	fi
fi
if ! pgrep -x usbhostfs_pc >/dev/null; then
	echo "$DIR|$ROMDIR" > /tmp/usbhostfs_pc.root
	(cd "$DIR" && nohup usbhostfs_pc "$DIR" "$ROMDIR" > "$LOG" 2>&1 &)
	for i in $(seq 1 20); do
		grep -q "Connected to device" "$LOG" 2>/dev/null && break
		sleep 0.5
	done
	if ! grep -q "Connected to device" "$LOG" 2>/dev/null; then
		echo "run: not connected to the PSP (check $LOG: PSPLink, USB, attach_psp.ps1 -Auto, udev rules)" >&2
		tail -5 "$LOG" >&2 || true
		exit 1
	fi
fi

if [ "$INSTALL_DEV" = "1" ]; then
	DEV="$ROOT/build-psp/devstub/EBOOT.PBP"
	[ -f "$DEV" ] || { echo "run: $DEV not found. Run bash tools/build.sh psp first" >&2; exit 2; }
	REL="${DEV#$DIR/}"
	# PSP go uses internal storage ef0:, 3000 uses Memory Stick ms0:
	DRV=ms0
	if psh "ls ef0:/PSP/GAME" | grep -qF " .."; then DRV=ef0; fi
	psh "mkdir $DRV:/PSP/GAME/REIREMDEV" >/dev/null || true
	PSH_TIMEOUT=30 psh "cp host0:/$REL $DRV:/PSP/GAME/REIREMDEV/EBOOT.PBP"
	echo "run: REIREM DEV installed ($DRV:/PSP/GAME/REIREMDEV) → run it from the PSP game menu"
	exit 0
fi

mods="$(psh modlist || true)"
[ -n "$mods" ] || die_frozen
if echo "$mods" | grep -q "Name: vsh_module"; then
	# At the XMB, launch REIREM DEV via the kernel launcher (psp/devstub/launch.c). USB reconnects on entering the game
	LAUNCH="$ROOT/build-psp/devstub/nbblaunch.prx"
	[ -f "$LAUNCH" ] || { echo "run: $LAUNCH not found. Run bash tools/build.sh psp first" >&2; exit 2; }
	echo "run: XMB → launching REIREM DEV"
	before="$(connects)"
	psh "ldstart host0:/${LAUNCH#$DIR/}" >/dev/null || true
	mods=""
	for i in $(seq 1 80); do
		sleep 0.5
		[ "$(connects)" -gt "$before" ] || continue
		mods="$(psh modlist || true)"
		echo "$mods" | grep -q "Name: reiremdev$" && break
	done
	if ! echo "$mods" | grep -q "Name: reiremdev$"; then
		echo "run: REIREM DEV did not start. Run it from the game menu (or --install-dev if missing)" >&2
		exit 1
	fi
	sleep 1
fi
if ! echo "$mods" | grep -q "Name: reiremdev$"; then
	echo "run: current game is not REIREM DEV. Quit with START and run \"REIREM DEV\" from the game menu" >&2
	exit 1
fi
rm -f "$DIR/rei_stop"
if echo "$mods" | grep -q "Name: reirem$"; then
	# Ask the previously loaded reirem to stop: debug builds check host0:/rei_stop every second and exit main,
	# then the C runtime frees the heap and the module unloads itself (kill leaks the heap; next load gets 0x800200D9)
	echo "run: cleaning up previous run (stop request)"
	touch "$DIR/rei_stop"
	gone=0
	for i in $(seq 1 30); do
		sleep 0.5
		if ! psh modlist | grep -q "Name: reirem$"; then gone=1; break; fi
	done
	rm -f "$DIR/rei_stop"
	if [ "$gone" = "0" ]; then
		echo "run: previous reirem did not unload. Quit the game with HOME and run \"REIREM DEV\" again" >&2
		exit 1
	fi
fi

# stdout/stderr receiver (the previous receiver is cleaned up, a new one attached)
pkill -f "nc 127.0.0.1 1000[23]" 2>/dev/null || true
: > "$OUT"
for port in 10002 10003; do
	(nohup nc 127.0.0.1 "$port" >> "$OUT" 2>/dev/null &)
done
sleep 0.5

if [ "${PSP_HWPROF:-0}" = "1" ]; then
	# Open hardware registers, enable perf counters (for app REI_HWPROF builds). Not reloaded if already loaded - reloading every run
	# piled up 30 copies, ran out of memory, and the next run could not open the ROM, stuck on "Init failed" (2026-10-13)
	if ! psh "modlist" | grep -q "Name: nbbkprof"; then
		psh "ldstart host0:/devstub/nbbkprof.prx" >/dev/null || true
	fi
fi
# The ME library kernel bridge (kcall.prx) stays after the app unloads. Unload it so the next run loads a fresh one
psh "modstun @kcall" >/dev/null || true
echo "run: running host0:/$NAME (output: $OUT)"
# Loading before the just-unloaded run's memory is fully freed fails (the app has only ~1MB free after start):
# check it is running; if not, wait and load again
for try in 1 2 3; do
	[ "$try" = "1" ] && sleep 1 || sleep 3
	PSH_TIMEOUT=15 psh "ldstart host0:/$NAME" || true
	sleep 1
	if psh modlist | grep -q "Name: reirem$"; then break; fi
	echo "run: load failed ($try/3), retrying" >&2
	[ "$try" = "3" ] && exit 1
done
if [ "$FOLLOW" = "1" ]; then
	tail -n +1 -f "$OUT"
fi

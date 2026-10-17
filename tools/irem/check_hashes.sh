#!/bin/bash
# Accuracy check for other Irem boards (non-M92): per set in the ROM folder, compare harness 2400-frame (autoplay) hashes with the baseline (irem_hashes.txt).
# With --render-thread, also check that the deferred-draw (render thread) path matches (ADR-018).
#
#   IREM_ROMS=/mnt/e/.../roms bash tools/irem/check_hashes.sh [--render-thread] [board or set ...]
#   Rebuild baseline: IREM_ROMS=... bash tools/irem/check_hashes.sh --update   (intended changes only, reason in PERF.md)
#
# Harness: HARNESS (default~/m92irem-pc/harness, cmake -S tools/harness -B ~/m92irem-pc -G Ninja -DCMAKE_BUILD_TYPE=Release)
# ROMs are not committed (docs/RULES.md ch. 6). Hashes are of video/audio and contain no ROM content.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
BASE="$DIR/irem_hashes.txt"
H="${HARNESS:-$HOME/m92irem-pc/harness}"
ROMS="${IREM_ROMS:?set IREM_ROMS to the ROM folder path}"
RT=""; UPDATE=0; SEL=()
for a in "$@"; do
	case "$a" in
	--render-thread) RT="--render-thread" ;;
	--update) UPDATE=1 ;;
	*) SEL+=("$a") ;;
	esac
done
want() { [ ${#SEL[@]} -eq 0 ] && return 0; for s in "${SEL[@]}"; do [ "$s" = "$1" ] || [ "$s" = "$2" ] && return 0; done; return 1; }
run() { timeout 300 "$H" --rom "$ROMS/$1.zip" --set "$1" --frames 2400 --autoplay $RT 2>&1 | grep -o 'video=[0-9a-f]* audio=[0-9a-f]*\|--render-thread:'; }

if [ $UPDATE = 1 ]; then # rebuild using the baseline file's set list as is
	tmp=$(mktemp)
	while read -r board set _; do
		r=$(run "$set")
		[ -n "$r" ] && echo "$board $set $r" >> "$tmp" || echo "run failed $set (dropped from baseline)"
	done < "$BASE"
	mv "$tmp" "$BASE"
	echo "baseline $(wc -l < "$BASE") sets → $BASE"
	exit 0
fi

same=0; diff=0; na=0
while read -r board set video audio; do
	want "$board" "$set" || continue
	got=$(run "$set")
	[ "$got" = "--render-thread:" ] && { na=$((na + 1)); continue; } # (driver without deferred-draw support)
	exp="$video $audio"
	[ -n "$RT" ] && { exp="$video"; got="${got%% audio=*}"; } # (render thread: video hash only; audio uses the same path)
	if [ "$got" = "$exp" ]; then same=$((same + 1)); else diff=$((diff + 1)); echo "diff $board $set: baseline $exp / now $got"; fi
done < "$BASE"
echo "same $same, diff $diff${RT:+, render thread unsupported $na}"
[ $diff = 0 ]

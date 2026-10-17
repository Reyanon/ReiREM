#!/usr/bin/env bash
# Measure recompilation gain (PC): compare V33 section ms/frame with recomp on/off in a REI_PROFILE harness and
# check both runs produce the same hash.   tools/recomp/bench.sh [rom]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
ROM="${1:-${REI_ROM:-$ROOT/nbbatman.zip}}"
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
cmake -B build-pc-prof -S tools/harness "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release -DREI_PROFILE=ON >/dev/null
cmake --build build-pc-prof -j >/dev/null
for args in "--autoplay --mode pipelined" "--mode pipelined"; do
	for rc in "" "--no-recomp"; do
		out=$(./build-pc-prof/harness --rom "$ROM" --frames 3600 $args $rc 2>&1)
		h=$(echo "$out" | grep -a '^frames=' | sed 's/frames=3600 //')
		v=$(echo "$out" | grep -a 'prof v33' | awk '{print $3}')
		printf '%-28s %-12s v33 %s ms/frame  %s\n' "$args" "${rc:-recomp}" "$v" "$h"
	done
done

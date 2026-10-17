#!/usr/bin/env bash
# M6 V33 recompiled code generation (user PC, user ROM). Output is not committed (docs/RULES.md ch. 6 rule 1).
#   tools/recomp/gen.sh [rom path [extra v33recomp.py args...]]   (default: REI_ROM or ./nbbatman.zip)
# 1) collect coverage with the REI_PCHIST harness (autoplay 3600 frames + attract with no input 3600 frames)
# 2) generate src/recomp/generated/v33_rc.inc with v33recomp.py
# Afterwards the PC harness and Vita builds turn REI_RECOMP=ON automatically when the generated file exists.
# Set: REI_SET=nbbatmanu REI_PARENT=nbbatman.zip tools/recomp/gen.sh nbbatmanu.zip  (PSP default is the US version)
# Generated code embeds a hash of the ROM regions; with a different ROM at runtime recompilation is disabled and the interpreter runs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
ROM="${1:-${REI_ROM:-$ROOT/nbbatman.zip}}"
[ $# -gt 0 ] && shift
COVERS=()
WORK="$ROOT/artifacts/recomp"
BUILD="$ROOT/build-cover"
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)

cmake -B "$BUILD" -S tools/harness "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release -DREI_PCHIST=ON -DREI_RECOMP=OFF >/dev/null
cmake --build "$BUILD" -j >/dev/null

SETARGS=()
[ -n "${REI_SET:-}" ] && SETARGS+=(--set "$REI_SET")
[ -n "${REI_PARENT:-}" ] && SETARGS+=(--parent "$REI_PARENT")
FRAMES="${REI_COVER_FRAMES:-3600}"
mkdir -p "$WORK/autoplay" "$WORK/attract"
"$BUILD/harness" --rom "$ROM" "${SETARGS[@]}" --frames "$FRAMES" --autoplay --mode pipelined --cover "$WORK/autoplay" >/dev/null 2>&1
"$BUILD/harness" --rom "$ROM" "${SETARGS[@]}" --frames "$FRAMES" --mode pipelined --cover "$WORK/attract" >/dev/null 2>&1
for d in "$WORK"/*/; do
	[ -f "$d/v33_cover.txt" ] && COVERS+=(--cover "$d/v33_cover.txt")
done

python3 tools/recomp/v33recomp.py --image "$WORK/autoplay/v33_image.bin" "${COVERS[@]}" \
	--out src/recomp/generated/v33_rc.inc "$@"

#!/usr/bin/env bash
# Linux/WSL build helper.
#   tools/build.sh vita [Release|Debug] [extra cmake args...]   → build-vita/ or build-vita-dbg/ (source vita/)
#   tools/build.sh psp  [Release|Debug] [extra cmake args...]   → build-psp/ (source psp/, needs PSPDEV)
#   tools/build.sh pc                                          → build-pc/harness
#   tools/build.sh hash [harness args...]                       → baseline hash measurement
#
# BUILD_DIR env var overrides the Vita build directory (e.g. for Release+log measurement)
# On Windows: wsl -d Ubuntu-24.04 -- bash tools/build.sh vita
# Uses ~/vitasdk if VITASDK is unset. ROM from REI_ROM or ./nbbatman.zip.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
export VITASDK="${VITASDK:-$HOME/vitasdk}"
export PATH="$VITASDK/bin:$PATH"
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)

case "${1:-}" in
vita)
	TYPE="${2:-Release}"
	shift $(( $# >= 2 ? 2 : 1 ))
	DIR=build-vita
	[ "$TYPE" = Release ] || DIR=build-vita-dbg
	DIR="${BUILD_DIR:-$DIR}"
	if [ ! -f "$VITASDK/share/vita.toolchain.cmake" ]; then
		echo "build: VitaSDK not found ($VITASDK)" >&2
		exit 1
	fi
	cmake -S vita -B "$DIR" "${GEN[@]}" -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
		-DCMAKE_BUILD_TYPE="$TYPE" "$@"
	cmake --build "$DIR" -j
	;;
psp)
	TYPE="${2:-Release}"
	shift $(( $# >= 2 ? 2 : 1 ))
	DIR="${BUILD_DIR:-build-psp}"
	export PSPDEV="${PSPDEV:-$HOME/pspdev}"
	export PATH="$PSPDEV/bin:$PATH"
	if [ ! -f "$PSPDEV/psp/share/pspdev.cmake" ]; then
		echo "build: PSPSDK (pspdev) not found ($PSPDEV)" >&2
		exit 1
	fi
	cmake -S psp -B "$DIR" "${GEN[@]}" -DCMAKE_TOOLCHAIN_FILE="$PSPDEV/psp/share/pspdev.cmake" \
		-DCMAKE_BUILD_TYPE="$TYPE" "$@"
	cmake --build "$DIR" -j
	;;
pc)
	cmake -B build-pc -S tools/harness "${GEN[@]}"
	cmake --build build-pc -j
	;;
hash)
	shift
	ROM="${REI_ROM:-$ROOT/nbbatman.zip}"
	./build-pc/harness --rom "$ROM" --frames 3600 --checkpoint 600 "$@"
	;;
*)
	sed -n '2,9p' "$0" >&2
	exit 2
	;;
esac

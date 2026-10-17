#!/usr/bin/env bash
# Replace only eboot.bin and relaunch on the Vita (docs/RULES.md 5-1).
# Requires the vitacompanion (1.07+) plugin resident on the Vita (FTP 1337, commands 1338).
# 1.07 closes apps with quit <TitleID> instead of destroy.
#
# Usage: VITA_IP=192.168.0.10 vita/tools/deploy.sh [build dir, default build-vita]
#
# On connection failure, report once and exit. No retry loop.
set -euo pipefail

TITLE_ID="M92A00001"
FTP_PORT=1337
CMD_PORT=1338
BUILD_DIR="${1:-build-vita}"
EBOOT="$BUILD_DIR/eboot.bin"

if [ -z "${VITA_IP:-}" ]; then
	echo "deploy: VITA_IP environment variable is not set" >&2
	exit 2
fi
if [ ! -f "$EBOOT" ]; then
	echo "deploy: $EBOOT not found. Build first" >&2
	exit 2
fi

# Send a vitacompanion command. Without nc (e.g. Windows Git Bash) use bash /dev/tcp.
vita_cmd() {
	if command -v nc >/dev/null 2>&1; then
		echo "$1" | nc -w 3 "$VITA_IP" "$CMD_PORT"
	else
		exec 3<>"/dev/tcp/$VITA_IP/$CMD_PORT" || return 1
		echo "$1" >&3
		exec 3>&-
	fi
}

# Close the running app first (releases the eboot.bin lock)
if ! vita_cmd "quit $TITLE_ID"; then
	echo "deploy: cannot connect to $VITA_IP:$CMD_PORT (check vitacompanion)" >&2
	exit 1
fi

# Right after quit the app may still hold eboot.bin, giving 550.
# Report connection failures immediately; retry only server rejects (curl 25 = 550), briefly, up to 3 times.
upload_ok=0
for attempt in 1 2 3; do
	sleep 1
	rc=0
	curl --silent --show-error --connect-timeout 5 \
		-T "$EBOOT" "ftp://$VITA_IP:$FTP_PORT/ux0:/app/$TITLE_ID/eboot.bin" || rc=$?
	if [ "$rc" -eq 0 ]; then
		upload_ok=1
		break
	fi
	[ "$rc" -eq 25 ] || break
	echo "deploy: upload rejected (550), waiting for app exit, retrying ($attempt/3)" >&2
done
if [ "$upload_ok" -ne 1 ]; then
	echo "deploy: FTP upload failed ($VITA_IP:$FTP_PORT)" >&2
	exit 1
fi

vita_cmd "launch $TITLE_ID"
echo "deploy: $EBOOT -> $VITA_IP ux0:/app/$TITLE_ID/ launched"

#!/usr/bin/env bash
# One-time setup for using PSPLink from WSL (run as root: sudo bash psp/tools/setup_psplink_wsl.sh)
# - install libusb-1.0 used by usbhostfs_pc/pspsh (incl. pspdev)
# - add a udev rule so regular users can open the USB device PSPLink exposes ("PSP" Type B, 054c:01c9)
# On Windows, attach the device to WSL with usbipd-win (psp/tools/attach_psp.ps1).
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
	echo "Run as root: sudo bash $0" >&2
	exit 1
fi

apt-get update -qq
apt-get install -y libusb-1.0-0

cat > /etc/udev/rules.d/60-psplink.rules <<'EOF'
# PSPLink (PSP USB "Type B")
SUBSYSTEM=="usb", ATTR{idVendor}=="054c", ATTR{idProduct}=="01c9", MODE="0666"
EOF
udevadm control --reload-rules
udevadm trigger --subsystem-match=usb --action=add

echo "Done: libusb installed, udev rule /etc/udev/rules.d/60-psplink.rules"
ls -l /dev/bus/usb/*/* 2>/dev/null || true

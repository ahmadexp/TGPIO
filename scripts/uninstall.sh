#!/bin/sh
set -eu

KREL=$(uname -r)
DEST="/lib/modules/${KREL}/extra/tgpio-ptp-input.ko"

if [ "$(id -u)" -ne 0 ]; then
	echo "Run as root: sudo $0" >&2
	exit 1
fi

rmmod tgpio_ptp_input 2>/dev/null || true
rm -f /etc/modprobe.d/tgpio-ptp-input.conf
rm -f /etc/modules-load.d/tgpio-ptp-input.conf
rm -f "${DEST}"
depmod "${KREL}"
echo "Uninstalled tgpio-ptp-input for ${KREL}"

#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "Run as root: sudo $0" >&2
	exit 1
fi

rmmod tgpio_ptp_input 2>/dev/null || true
rmmod tgpio_platform 2>/dev/null || true
rmmod pps_gen_tio 2>/dev/null || true
rmmod pps_gen_core 2>/dev/null || true

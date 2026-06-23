#!/bin/sh
set -eu

echo "== modules =="
lsmod | grep -E '^(tgpio_ptp_input|tgpio_platform|pps_gen_tio|pps_gen_core|pps_core|ptp)' || true

echo
echo "== legacy output platform devices =="
find /sys/bus/platform/devices -maxdepth 1 -name 'intel-pps-gen-tio*' -print 2>/dev/null || true

echo
echo "== ptp devices =="
for dev in /sys/class/ptp/ptp*; do
	[ -e "${dev}" ] || continue
	name=$(cat "${dev}/clock_name" 2>/dev/null || true)
	printf '%s %s\n' "${dev}" "${name}"
done

echo
echo "== tgpio parameters =="
for param in timestamp_mode output_polarity art_frequency tsc_art_numerator \
	tsc_art_denominator hardware_timestamps; do
	path="/sys/module/tgpio_ptp_input/parameters/${param}"
	[ -r "${path}" ] || continue
	printf '%s=%s\n' "${param}" "$(cat "${path}")"
done

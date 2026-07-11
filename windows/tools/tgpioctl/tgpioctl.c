// Licensed under the TGPIO Non-Commercial License (see LICENSE).
// Commercial use requires the prior written permission of Ahmad Byagowi.

/*
 * tgpioctl -- user-mode control tool for the TGPIO Windows driver.
 * Copyright (c) 2026 Ahmad Byagowi
 *
 * The driver deals only in raw ART cycles; every wall-clock conversion in
 * this tool goes through the driver's crosststamp (TSC-bracketed
 * KeQuerySystemTimePrecise) and the nominal crystal frequency from CPUID
 * leaf 0x15. Good to the crystal's ppm error -- the right tool for
 * bring-up; discipline comes later.
 *
 *   tgpioctl info
 *   tgpioctl status <block>
 *   tgpioctl input <block> [rising|falling|both]
 *   tgpioctl watch <block>
 *   tgpioctl output <block> <period, e.g. 1s|500ms|1000000ns> [align]
 *   tgpioctl stop <block>
 *   tgpioctl invert <block>
 */

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>

#include "../../include/tgpio_ioctl.h"

#define HNS_PER_SEC 10000000ull /* 100 ns units per second */

static HANDLE tgpio_open(void)
{
	HANDLE h = CreateFileW(TGPIO_USER_PATH, GENERIC_READ | GENERIC_WRITE,
			       0, NULL, OPEN_EXISTING, 0, NULL);

	if (h == INVALID_HANDLE_VALUE) {
		fprintf(stderr,
			"tgpioctl: cannot open %ls (error %lu). Is tgpio.sys "
			"installed and started?\n",
			TGPIO_USER_PATH, GetLastError());
		exit(1);
	}
	return h;
}

static int tgpio_ioctl(HANDLE h, DWORD code, void *in, DWORD in_len,
		       void *out, DWORD out_len)
{
	DWORD returned = 0;

	if (!DeviceIoControl(h, code, in, in_len, out, out_len, &returned,
			     NULL)) {
		fprintf(stderr, "tgpioctl: ioctl 0x%lx failed, error %lu\n",
			code, GetLastError());
		return -1;
	}
	return 0;
}

static unsigned __int64 muldiv_u64(unsigned __int64 value,
				   unsigned __int64 mul, unsigned __int64 div)
{
	unsigned __int64 hi;
	unsigned __int64 lo = _umul128(value, mul, &hi);
	unsigned __int64 rem;

	if (hi >= div)
		return ~0ull;
	return _udiv128(hi, lo, div, &rem);
}

static int get_info(HANDLE h, struct tgpio_win_info *info)
{
	return tgpio_ioctl(h, TGPIO_IOCTL_GET_INFO, NULL, 0, info,
			   sizeof(*info));
}

static int get_crosststamp(HANDLE h, struct tgpio_win_crosststamp *xt)
{
	return tgpio_ioctl(h, TGPIO_IOCTL_CROSSTSTAMP, NULL, 0, xt,
			   sizeof(*xt));
}

/* ART capture -> UTC 100ns, through one crosststamp and the nominal rate. */
static unsigned __int64 art_to_hns(const struct tgpio_win_info *info,
				   const struct tgpio_win_crosststamp *xt,
				   unsigned __int64 art)
{
	if (art >= xt->art_cycles)
		return xt->systime_100ns +
		       muldiv_u64(art - xt->art_cycles, HNS_PER_SEC,
				  info->art_frequency_hz);
	return xt->systime_100ns -
	       muldiv_u64(xt->art_cycles - art, HNS_PER_SEC,
			  info->art_frequency_hz);
}

static void print_utc(unsigned __int64 hns)
{
	FILETIME ft;
	SYSTEMTIME st;

	ft.dwLowDateTime = (DWORD)hns;
	ft.dwHighDateTime = (DWORD)(hns >> 32);
	if (!FileTimeToSystemTime(&ft, &st)) {
		printf("(invalid time)");
		return;
	}
	printf("%04u-%02u-%02u %02u:%02u:%02u.%07llu UTC", st.wYear,
	       st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
	       hns % HNS_PER_SEC);
}

static unsigned __int64 parse_period_ns(const char *arg)
{
	char *end = NULL;
	unsigned __int64 value = _strtoui64(arg, &end, 10);

	if (!end || end == arg)
		return 0;
	if (!strcmp(end, "s"))
		return value * 1000000000ull;
	if (!strcmp(end, "ms"))
		return value * 1000000ull;
	if (!strcmp(end, "us"))
		return value * 1000ull;
	if (!strcmp(end, "ns") || !*end)
		return value;
	return 0;
}

static int cmd_info(HANDLE h)
{
	struct tgpio_win_info info;
	struct tgpio_win_crosststamp xt;
	ULONG i;

	if (get_info(h, &info) || get_crosststamp(h, &xt))
		return 1;

	printf("ART frequency:   %llu Hz\n", info.art_frequency_hz);
	printf("TSC/ART ratio:   %u/%u\n", info.tsc_art_numerator,
	       info.tsc_art_denominator);
	printf("TSC adjust:      %llu\n", info.tsc_adjust);
	printf("Blocks:          %u\n", info.block_count);
	for (i = 0; i < info.block_count; i++)
		printf("  block %lu MMIO:  0x%llx\n", i, info.mmio_base[i]);
	printf("ART now:         %llu cycles\n", xt.art_cycles);
	printf("Crosststamp:     tsc bracket %llu cycles, system time ",
	       xt.tsc_after - xt.tsc_before);
	print_utc(xt.systime_100ns);
	printf("\n");
	return 0;
}

static int cmd_status(HANDLE h, ULONG block)
{
	static const char *modes[] = { "off", "input", "output" };
	struct tgpio_win_block_status st;

	if (tgpio_ioctl(h, TGPIO_IOCTL_GET_BLOCK, &block, sizeof(block), &st,
			sizeof(st)))
		return 1;

	printf("block %lu: mode=%s ctl=0x%02x compv=%llu piv=%llu "
	       "events=%llu flop=%s\n",
	       block, st.mode <= 2 ? modes[st.mode] : "?", st.ctl, st.compv,
	       st.piv, st.event_count, st.flop_high ? "high" : "low");
	return 0;
}

static int cmd_input(HANDLE h, ULONG block, const char *edge_arg)
{
	struct tgpio_win_set_input in = { 0 };

	in.block = block;
	in.enable = 1;
	in.edge = TGPIO_WIN_EDGE_RISING;
	if (edge_arg) {
		if (!strcmp(edge_arg, "falling"))
			in.edge = TGPIO_WIN_EDGE_FALLING;
		else if (!strcmp(edge_arg, "both"))
			in.edge = TGPIO_WIN_EDGE_BOTH;
		else if (strcmp(edge_arg, "rising")) {
			fprintf(stderr, "tgpioctl: bad edge '%s'\n", edge_arg);
			return 1;
		}
	}

	if (tgpio_ioctl(h, TGPIO_IOCTL_SET_INPUT, &in, sizeof(in), NULL, 0))
		return 1;
	printf("block %lu: input capture enabled\n", block);
	return 0;
}

static int cmd_watch(HANDLE h, ULONG block)
{
	struct tgpio_win_info info;
	struct tgpio_win_crosststamp xt;
	struct tgpio_win_capture cap;
	unsigned __int64 last_count = 0;
	int first = 1;

	if (get_info(h, &info) || get_crosststamp(h, &xt))
		return 1;

	printf("watching block %lu (Ctrl-C to stop)\n", block);
	for (;;) {
		if (tgpio_ioctl(h, TGPIO_IOCTL_READ_CAPTURE, &block,
				sizeof(block), &cap, sizeof(cap)))
			return 1;
		if (first) {
			last_count = cap.event_count;
			first = 0;
		} else if (cap.event_count != last_count) {
			unsigned __int64 hns =
				art_to_hns(&info, &xt, cap.art_cycles);

			printf("event %llu (+%llu)  art=%llu  ",
			       cap.event_count,
			       cap.event_count - last_count, cap.art_cycles);
			print_utc(hns);
			printf("\n");
			last_count = cap.event_count;
		}
		Sleep(10);
	}
}

static int cmd_output(HANDLE h, ULONG block, const char *period_arg,
		      int align_second)
{
	struct tgpio_win_info info;
	struct tgpio_win_crosststamp xt;
	struct tgpio_win_start_output out = { 0 };
	unsigned __int64 period_ns = parse_period_ns(period_arg);
	unsigned __int64 period_cycles;

	if (!period_ns) {
		fprintf(stderr, "tgpioctl: bad period '%s'\n", period_arg);
		return 1;
	}
	if (get_info(h, &info) || get_crosststamp(h, &xt))
		return 1;

	period_cycles =
		muldiv_u64(period_ns, info.art_frequency_hz, 1000000000ull);
	if (period_cycles < 2) {
		fprintf(stderr, "tgpioctl: period below 2 ART cycles\n");
		return 1;
	}

	/* Symmetric halves; an odd cycle count rounds down one cycle. */
	out.block = block;
	out.high_cycles = period_cycles / 2;
	out.low_cycles = period_cycles / 2;

	if (align_second) {
		/* First rising edge on the next UTC second boundary at
		 * least 100 ms away, so the driver's late-arm guard never
		 * has to shift the phase.
		 */
		unsigned __int64 target =
			(xt.systime_100ns / HNS_PER_SEC + 1) * HNS_PER_SEC;

		if (target - xt.systime_100ns < HNS_PER_SEC / 10)
			target += HNS_PER_SEC;
		out.first_edge_art =
			xt.art_cycles + muldiv_u64(target - xt.systime_100ns,
						   info.art_frequency_hz,
						   HNS_PER_SEC);
		printf("first rising edge aligned to ");
		print_utc(target);
		printf("\n");
	}

	if (tgpio_ioctl(h, TGPIO_IOCTL_START_OUTPUT, &out, sizeof(out), NULL,
			0))
		return 1;
	printf("block %lu: periodic output started, period %llu ns "
	       "(%llu ART cycles, actual %llu ns)\n",
	       block, period_ns, period_cycles,
	       muldiv_u64(period_cycles, 1000000000ull,
			  info.art_frequency_hz));
	return 0;
}

static int cmd_simple(HANDLE h, DWORD code, ULONG block, const char *verb)
{
	if (tgpio_ioctl(h, code, &block, sizeof(block), NULL, 0))
		return 1;
	printf("block %lu: %s\n", block, verb);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: tgpioctl info\n"
		"       tgpioctl status <block>\n"
		"       tgpioctl input <block> [rising|falling|both]\n"
		"       tgpioctl watch <block>\n"
		"       tgpioctl output <block> <period> [align]\n"
		"       tgpioctl stop <block>\n"
		"       tgpioctl invert <block>\n"
		"period examples: 1s, 500ms, 100us, 1000000ns\n");
	exit(2);
}

int main(int argc, char **argv)
{
	HANDLE h;
	ULONG block;
	int ret = 1;

	if (argc < 2)
		usage();

	h = tgpio_open();

	if (!strcmp(argv[1], "info")) {
		ret = cmd_info(h);
	} else if (argc >= 3) {
		block = strtoul(argv[2], NULL, 10);
		if (!strcmp(argv[1], "status"))
			ret = cmd_status(h, block);
		else if (!strcmp(argv[1], "input"))
			ret = cmd_input(h, block, argc > 3 ? argv[3] : NULL);
		else if (!strcmp(argv[1], "watch"))
			ret = cmd_watch(h, block);
		else if (!strcmp(argv[1], "output") && argc >= 4)
			ret = cmd_output(h, block, argv[3],
					 argc > 4 &&
						 !strcmp(argv[4], "align"));
		else if (!strcmp(argv[1], "stop"))
			ret = cmd_simple(h, TGPIO_IOCTL_STOP_OUTPUT, block,
					 "output stopped");
		else if (!strcmp(argv[1], "invert"))
			ret = cmd_simple(h, TGPIO_IOCTL_INVERT_OUTPUT, block,
					 "output polarity inverted");
		else
			usage();
	} else {
		usage();
	}

	CloseHandle(h);
	return ret;
}

// Licensed under the TGPIO Non-Commercial License (see LICENSE).
// Commercial use requires the prior written permission of Ahmad Byagowi.

/*
 * Intel Time-Aware GPIO -- Windows driver IOCTL interface.
 * Copyright (c) 2026 Ahmad Byagowi
 *
 * Shared between tgpio.sys (kernel) and tgpioctl.exe (user mode). All
 * absolute times cross this boundary as raw ART cycles; user mode converts
 * to and from wall time with TGPIO_IOCTL_CROSSTSTAMP. The kernel stays a
 * thin, deterministic hardware layer with no clock model of its own.
 */

#ifndef TGPIO_IOCTL_H
#define TGPIO_IOCTL_H

#define TGPIO_SYMLINK_NAME  L"\\DosDevices\\TGPIO"
#define TGPIO_USER_PATH     L"\\\\.\\TGPIO"

#define TGPIO_WIN_MAX_BLOCKS 2

#define TGPIO_IOCTL(index) \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (index), METHOD_BUFFERED, \
		 FILE_ANY_ACCESS)

#define TGPIO_IOCTL_GET_INFO	  TGPIO_IOCTL(0)
#define TGPIO_IOCTL_CROSSTSTAMP	  TGPIO_IOCTL(1)
#define TGPIO_IOCTL_SET_INPUT	  TGPIO_IOCTL(2)
#define TGPIO_IOCTL_READ_CAPTURE  TGPIO_IOCTL(3)
#define TGPIO_IOCTL_START_OUTPUT  TGPIO_IOCTL(4)
#define TGPIO_IOCTL_STOP_OUTPUT	  TGPIO_IOCTL(5)
#define TGPIO_IOCTL_INVERT_OUTPUT TGPIO_IOCTL(6)
#define TGPIO_IOCTL_GET_BLOCK	  TGPIO_IOCTL(7)

enum tgpio_win_edge {
	TGPIO_WIN_EDGE_RISING = 0,
	TGPIO_WIN_EDGE_FALLING = 1,
	TGPIO_WIN_EDGE_BOTH = 2,
};

enum tgpio_win_block_mode {
	TGPIO_WIN_BLOCK_OFF = 0,
	TGPIO_WIN_BLOCK_INPUT = 1,
	TGPIO_WIN_BLOCK_OUTPUT = 2,
};

#pragma pack(push, 8)

/* TGPIO_IOCTL_GET_INFO: out only. */
struct tgpio_win_info {
	unsigned __int64 art_frequency_hz; /* CPUID 15h ECX, 0 if unknown */
	unsigned __int32 tsc_art_numerator; /* CPUID 15h EBX */
	unsigned __int32 tsc_art_denominator; /* CPUID 15h EAX */
	unsigned __int64 tsc_adjust; /* IA32_TSC_ADJUST at driver load */
	unsigned __int32 block_count;
	unsigned __int32 reserved;
	unsigned __int64 mmio_base[TGPIO_WIN_MAX_BLOCKS];
};

/*
 * TGPIO_IOCTL_CROSSTSTAMP: out only. The system time is sampled between two
 * TSC reads; art is derived from the midpoint TSC. systime is
 * KeQuerySystemTimePrecise in 100 ns units since 1601 (FILETIME epoch).
 */
struct tgpio_win_crosststamp {
	unsigned __int64 tsc_before;
	unsigned __int64 tsc_after;
	unsigned __int64 systime_100ns;
	unsigned __int64 art_cycles; /* ART at the midpoint TSC */
};

/* TGPIO_IOCTL_SET_INPUT: in only. enable=0 disables capture. */
struct tgpio_win_set_input {
	unsigned __int32 block;
	unsigned __int32 enable;
	unsigned __int32 edge; /* enum tgpio_win_edge */
	unsigned __int32 reserved;
};

/* TGPIO_IOCTL_READ_CAPTURE: in block index (u32), out this struct. */
struct tgpio_win_capture {
	unsigned __int64 event_count; /* hardware event counter */
	unsigned __int64 art_cycles; /* capture timestamp, ART domain */
	unsigned __int64 tsc_now; /* TSC sampled just after the read */
	unsigned __int64 systime_100ns; /* system time paired with tsc_now */
};

/*
 * TGPIO_IOCTL_START_OUTPUT: in only. Periodic toggle output.
 * first_edge_art = 0 lets the driver start ~50 ms from now. Non-zero values
 * are absolute ART cycles for the first rising edge; the driver pushes a
 * late start forward by whole periods so the phase grid is preserved.
 * low_cycles = 0 means symmetric (low = high).
 */
struct tgpio_win_start_output {
	unsigned __int32 block;
	unsigned __int32 reserved;
	unsigned __int64 high_cycles; /* high half-period, ART cycles */
	unsigned __int64 low_cycles; /* low half-period, 0 = high_cycles */
	unsigned __int64 first_edge_art; /* absolute ART, 0 = auto */
};

/* TGPIO_IOCTL_STOP_OUTPUT / INVERT_OUTPUT: in block index (u32). */

/* TGPIO_IOCTL_GET_BLOCK: in block index (u32), out this struct. */
struct tgpio_win_block_status {
	unsigned __int32 mode; /* enum tgpio_win_block_mode */
	unsigned __int32 ctl; /* raw TGPIOCTL */
	unsigned __int64 compv;
	unsigned __int64 piv;
	unsigned __int64 event_count;
	unsigned __int32 flop_high; /* tracked output level mirror */
	unsigned __int32 reserved;
};

#pragma pack(pop)

#endif /* TGPIO_IOCTL_H */

// Licensed under the TGPIO Non-Commercial License (see LICENSE).
// Commercial use requires the prior written permission of Ahmad Byagowi.

/*
 * Intel Time-Aware GPIO -- Windows KMDF driver, shared definitions.
 * Copyright (c) 2026 Ahmad Byagowi
 *
 * Port of the hardware layer of the Linux tgpio-ptp-input driver. The
 * register map, control bits, and every programming sequence follow the
 * Linux driver, which is the reference for what this hardware honors.
 */

#ifndef TGPIO_WIN_DRIVER_H
#define TGPIO_WIN_DRIVER_H

#include <ntddk.h>
#include <wdf.h>
#include <intrin.h>

#include "../include/tgpio_ioctl.h"

#ifndef MAXULONG64
#define MAXULONG64 0xffffffffffffffffull
#endif

/* Register map (byte offsets from each block's MMIO base). */
#define TGPIOCTL	0x00
#define TGPIOCOMPV31_0	0x10
#define TGPIOCOMPV63_32 0x14
#define TGPIOPIV31_0	0x18
#define TGPIOPIV63_32	0x1c
#define TGPIOTCV31_0	0x20
#define TGPIOTCV63_32	0x24
#define TGPIOECCV31_0	0x28
#define TGPIOECCV63_32	0x2c
#define TGPIOEC31_0	0x30
#define TGPIOEC63_32	0x34

#define TGPIOCTL_EN	    0x01u
#define TGPIOCTL_DIR	    0x02u
#define TGPIOCTL_EP_MASK    0x0cu
#define TGPIOCTL_EP_RISING  (0u << 2)
#define TGPIOCTL_EP_FALLING (1u << 2)
#define TGPIOCTL_EP_TOGGLE  (2u << 2)
#define TGPIOCTL_PM	    0x10u

/* Compare values fire this many ART cycles late; pre-subtract. */
#define TGPIO_ART_HW_DELAY_CYCLES 2

/* Late-arm guard: first edge must sit at least this far ahead of ART now. */
#define TGPIO_ARM_MARGIN_NS 2000000ull /* 2 ms */
/* Auto start (first_edge_art == 0): first edge this far from now. */
#define TGPIO_AUTO_START_NS 50000000ull /* 50 ms */

#define TGPIO_DEFAULT_ADDR0	0xFE001210ull
#define TGPIO_DEFAULT_ADDR1	0xFE001310ull
#define TGPIO_DEFAULT_MMIO_SIZE 0x38ul

#define TGPIO_POOL_TAG 'IPGT'

struct tgpio_win_block {
	volatile UCHAR *regs; /* mapped MMIO, NULL if absent */
	PHYSICAL_ADDRESS phys;
	ULONG mmio_size;
	enum tgpio_win_block_mode mode;

	/*
	 * Output level mirror. The hardware flop holds the level of the last
	 * generated toggle, survives disable, and has no readback; this
	 * software shadow is the only source of polarity truth, folded
	 * forward from COMPV movement (COMPV advances by PIV per edge).
	 */
	BOOLEAN flop_high;
	unsigned __int64 hw_piv; /* PIV live in hardware, 0 when stopped */
	unsigned __int64 ckpt_compv; /* COMPV at the last fold */
	unsigned __int64 high_cycles;
	unsigned __int64 low_cycles;
};

/* WDF context macros token-paste the type name: keep it a bare typedef. */
typedef struct tgpio_win_device {
	WDFDEVICE wdf_device;
	WDFSPINLOCK lock; /* serializes all hardware access */
	struct tgpio_win_block block[TGPIO_WIN_MAX_BLOCKS];
	ULONG block_count;

	unsigned __int64 art_frequency_hz;
	ULONG tsc_art_numerator;
	ULONG tsc_art_denominator;
	unsigned __int64 tsc_adjust;
} TGPIO_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TGPIO_DEVICE_CONTEXT,
				   tgpio_win_get_context)

/* hw.c -- the only MMIO. Callers hold dev->lock. */
ULONG tgpio_hw_read_ctl(struct tgpio_win_block *block);
unsigned __int64 tgpio_hw_read_compv(struct tgpio_win_block *block);
unsigned __int64 tgpio_hw_read_piv(struct tgpio_win_block *block);
void tgpio_hw_read_capture(struct tgpio_win_block *block,
			   unsigned __int64 *event_count,
			   unsigned __int64 *art_cycles);
unsigned __int64 tgpio_hw_read_event_count(struct tgpio_win_block *block);
void tgpio_hw_set_input(struct tgpio_win_block *block, BOOLEAN enable,
			ULONG edge_bits);
NTSTATUS tgpio_hw_start_output(struct tgpio_win_device *dev,
			       struct tgpio_win_block *block,
			       unsigned __int64 high_cycles,
			       unsigned __int64 low_cycles,
			       unsigned __int64 first_edge_art);
void tgpio_hw_stop_output(struct tgpio_win_block *block);
void tgpio_hw_invert_output(struct tgpio_win_block *block);
void tgpio_hw_flop_fold(struct tgpio_win_block *block);

/* art.c -- TSC/ART/system time. No MMIO. */
NTSTATUS tgpio_art_detect(struct tgpio_win_device *dev);
unsigned __int64 tgpio_art_from_tsc(const struct tgpio_win_device *dev,
				    unsigned __int64 tsc);
unsigned __int64 tgpio_art_now(const struct tgpio_win_device *dev);
unsigned __int64 tgpio_art_ns_to_cycles(const struct tgpio_win_device *dev,
					unsigned __int64 ns);
void tgpio_art_crosststamp(const struct tgpio_win_device *dev,
			   struct tgpio_win_crosststamp *xt);

/* ioctl.c */
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL tgpio_evt_ioctl;

#endif /* TGPIO_WIN_DRIVER_H */

// Licensed under the TGPIO Non-Commercial License (see LICENSE).
// Commercial use requires the prior written permission of Ahmad Byagowi.

/*
 * Intel Time-Aware GPIO -- Windows KMDF driver, hardware layer.
 * Copyright (c) 2026 Ahmad Byagowi
 *
 * Direct port of the Linux driver's MMIO sequences. Hardware facts this
 * code depends on (all verified on the Linux reference):
 *
 *  - Single-shot compares never fire; only the periodic toggle engine
 *    (EP=toggle, PM=1) generates edges reliably.
 *  - COMPV reads back live and advances by PIV on every generated edge,
 *    so 64-bit reads must retry until the high word is stable.
 *  - The output level flop has no readback and no reset; polarity is
 *    tracked in software by folding COMPV movement (one toggle per PIV).
 *  - A bare COMPV rewrite on a running block can emit a runt and register
 *    a phantom event; rewrites happen with PM cleared (EN kept, the line
 *    holds its level).
 *  - Hot PIV writes are safe only while the 64-bit high word is unchanged
 *    (two 32-bit stores; a reload between them would see a torn value).
 *  - Compare values fire ~2 ART cycles late; pre-subtract.
 *  - Register writes while EN is set are otherwise not honored: every
 *    configuration change goes through a full disable/arm cycle.
 */

#include "tgpio.h"

static ULONG tgpio_readl(struct tgpio_win_block *block, ULONG offset)
{
	return READ_REGISTER_ULONG((volatile ULONG *)(block->regs + offset));
}

static void tgpio_writel(struct tgpio_win_block *block, ULONG offset,
			 ULONG value)
{
	WRITE_REGISTER_ULONG((volatile ULONG *)(block->regs + offset), value);
}

ULONG tgpio_hw_read_ctl(struct tgpio_win_block *block)
{
	return tgpio_readl(block, TGPIOCTL);
}

static void tgpio_write_ctl(struct tgpio_win_block *block, ULONG value)
{
	tgpio_writel(block, TGPIOCTL, value);
}

static void tgpio_write_compv(struct tgpio_win_block *block,
			      unsigned __int64 value)
{
	tgpio_writel(block, TGPIOCOMPV63_32, (ULONG)(value >> 32));
	tgpio_writel(block, TGPIOCOMPV31_0, (ULONG)value);
}

static void tgpio_write_piv(struct tgpio_win_block *block,
			    unsigned __int64 value)
{
	tgpio_writel(block, TGPIOPIV63_32, (ULONG)(value >> 32));
	tgpio_writel(block, TGPIOPIV31_0, (ULONG)value);
}

/* COMPV advances by PIV per generated edge: retry torn 64-bit reads. */
unsigned __int64 tgpio_hw_read_compv(struct tgpio_win_block *block)
{
	ULONG hi = tgpio_readl(block, TGPIOCOMPV63_32);
	ULONG lo;
	ULONG prev_hi;

	do {
		prev_hi = hi;
		lo = tgpio_readl(block, TGPIOCOMPV31_0);
		hi = tgpio_readl(block, TGPIOCOMPV63_32);
	} while (hi != prev_hi);

	return ((unsigned __int64)hi << 32) | lo;
}

unsigned __int64 tgpio_hw_read_piv(struct tgpio_win_block *block)
{
	unsigned __int64 lo = tgpio_readl(block, TGPIOPIV31_0);
	unsigned __int64 hi = tgpio_readl(block, TGPIOPIV63_32);

	return (hi << 32) | lo;
}

/* Reading TCV low latches the capture timestamp and event count. */
void tgpio_hw_read_capture(struct tgpio_win_block *block,
			   unsigned __int64 *event_count,
			   unsigned __int64 *art_cycles)
{
	unsigned __int64 tcv_lo = tgpio_readl(block, TGPIOTCV31_0);
	unsigned __int64 count_hi = tgpio_readl(block, TGPIOECCV63_32);
	unsigned __int64 count_lo = tgpio_readl(block, TGPIOECCV31_0);
	unsigned __int64 tcv_hi = tgpio_readl(block, TGPIOTCV63_32);

	*event_count = (count_hi << 32) | count_lo;
	*art_cycles = (tcv_hi << 32) | tcv_lo;
}

unsigned __int64 tgpio_hw_read_event_count(struct tgpio_win_block *block)
{
	unsigned __int64 count;
	unsigned __int64 art;

	tgpio_hw_read_capture(block, &count, &art);
	return count;
}

/*
 * Fold the toggles generated since the last checkpoint into the tracked
 * level flop. COMPV advances by exactly PIV per edge and the checkpoint is
 * refreshed at every PIV change and COMPV rewrite, so the distance is an
 * exact multiple of the live PIV.
 */
void tgpio_hw_flop_fold(struct tgpio_win_block *block)
{
	unsigned __int64 compv;
	unsigned __int64 events;

	if (!block->hw_piv)
		return;

	compv = tgpio_hw_read_compv(block);
	if (compv < block->ckpt_compv)
		return;

	events = (compv - block->ckpt_compv) / block->hw_piv;
	if (events & 1)
		block->flop_high = !block->flop_high;
	block->ckpt_compv = compv;
}

/* Input: disable, program edge and direction, baseline, enable. */
void tgpio_hw_set_input(struct tgpio_win_block *block, BOOLEAN enable,
			ULONG edge_bits)
{
	ULONG ctrl = tgpio_hw_read_ctl(block) & ~TGPIOCTL_EN;

	tgpio_write_ctl(block, ctrl);

	if (!enable)
		return;

	ctrl = (ctrl & ~TGPIOCTL_EP_MASK) | (edge_bits & TGPIOCTL_EP_MASK);
	ctrl |= TGPIOCTL_DIR;
	tgpio_write_ctl(block, ctrl);

	tgpio_write_ctl(block, ctrl | TGPIOCTL_EN);
}

/*
 * Rewrite the pending compare of a running block with the periodic engine
 * paused: clearing PM (EN stays set, so the line holds its level) stops the
 * comparator for the few writes, avoiding runts and phantom parity flips.
 */
static void tgpio_hw_write_compv_paused(struct tgpio_win_block *block,
					unsigned __int64 compv)
{
	ULONG ctrl = tgpio_hw_read_ctl(block);

	tgpio_write_ctl(block, ctrl & ~TGPIOCTL_PM);
	tgpio_write_compv(block, compv);
	tgpio_write_ctl(block, ctrl);
	block->ckpt_compv = compv;
}

/*
 * Switch a block to autonomous hardware periodic output. Mirrors
 * tgpio_arm_hardware_periodic in the Linux driver: full disable, flop fold,
 * register clear, slot selection from the tracked flop, late-arm guard,
 * then PIV, COMPV, and enable -- in that order.
 */
NTSTATUS tgpio_hw_start_output(struct tgpio_win_device *dev,
			       struct tgpio_win_block *block,
			       unsigned __int64 high_cycles,
			       unsigned __int64 low_cycles,
			       unsigned __int64 first_edge_art)
{
	unsigned __int64 period_cycles;
	unsigned __int64 now_art;
	unsigned __int64 deadline;
	unsigned __int64 initial_piv;
	unsigned __int64 first;
	ULONG ctrl;

	if (!high_cycles || !low_cycles ||
	    high_cycles > MAXULONG64 / 4 || low_cycles > MAXULONG64 / 4)
		return STATUS_INVALID_PARAMETER;
	period_cycles = high_cycles + low_cycles;

	ctrl = tgpio_hw_read_ctl(block) & ~TGPIOCTL_EN;

	/* Stop events, then fold pending toggles before clearing registers:
	 * a re-arm that pre-empts a running waveform must not lose parity.
	 */
	tgpio_write_ctl(block, ctrl);
	tgpio_hw_flop_fold(block);
	tgpio_write_compv(block, 0);
	tgpio_write_piv(block, 0);

	ctrl &= ~(TGPIOCTL_DIR | TGPIOCTL_EP_MASK | TGPIOCTL_PM);
	tgpio_write_ctl(block, ctrl);

	now_art = tgpio_art_now(dev);
	if (!first_edge_art)
		first_edge_art =
			now_art + tgpio_art_ns_to_cycles(dev,
							 TGPIO_AUTO_START_NS);
	first = first_edge_art;
	if (first > TGPIO_ART_HW_DELAY_CYCLES)
		first -= TGPIO_ART_HW_DELAY_CYCLES;

	/*
	 * The flop cannot be loaded or read. If the tracked level is high,
	 * the first toggle must be a falling edge, programmed one low-time
	 * before the requested rising edge so rising edges stay on grid.
	 */
	if (block->flop_high) {
		if (first > low_cycles)
			first -= low_cycles;
		else
			first += high_cycles;
	}

	/*
	 * Late-arm guard: a compare already in the past at enable fires
	 * immediately and inverts the waveform. Push forward by whole
	 * periods (parity-preserving) until safely ahead of the counter.
	 */
	deadline = now_art + tgpio_art_ns_to_cycles(dev, TGPIO_ARM_MARGIN_NS);
	if (first < deadline) {
		unsigned __int64 periods =
			(deadline - first) / period_cycles + 1;

		if (periods > (MAXULONG64 - first) / period_cycles)
			return STATUS_INTEGER_OVERFLOW;
		first += periods * period_cycles;
	}

	/* The reload at the pending edge consumes PIV: a pending rise needs
	 * the high time, a pending fall the low time.
	 */
	initial_piv = block->flop_high ? low_cycles : high_cycles;

	ctrl |= TGPIOCTL_EP_TOGGLE | TGPIOCTL_PM;
	tgpio_write_ctl(block, ctrl);

	tgpio_write_piv(block, initial_piv);
	tgpio_write_compv(block, first);

	block->high_cycles = high_cycles;
	block->low_cycles = low_cycles;
	block->hw_piv = initial_piv;
	block->ckpt_compv = first;

	tgpio_write_ctl(block, ctrl | TGPIOCTL_EN);
	return STATUS_SUCCESS;
}

void tgpio_hw_stop_output(struct tgpio_win_block *block)
{
	ULONG ctrl = tgpio_hw_read_ctl(block);

	/* Stop event generation first, then fold the final COMPV state into
	 * the level flop; an edge between the fold and disable would
	 * otherwise be lost from the parity.
	 */
	tgpio_write_ctl(block, ctrl & ~TGPIOCTL_EN);
	tgpio_hw_flop_fold(block);
	tgpio_write_compv(block, 0);
	tgpio_write_piv(block, 0);
	tgpio_write_ctl(block, ctrl & ~(TGPIOCTL_EN | TGPIOCTL_PM));
	block->hw_piv = 0;
	block->ckpt_compv = 0;
}

/*
 * External calibration says the tracked level is inverted: fix the belief
 * and shift the pending compare by half a period (one stretched half-cycle,
 * no glitch) so rising edges return to the requested grid.
 */
void tgpio_hw_invert_output(struct tgpio_win_block *block)
{
	unsigned __int64 compv;

	block->flop_high = !block->flop_high;

	if (!block->hw_piv)
		return;

	tgpio_hw_flop_fold(block);
	compv = tgpio_hw_read_compv(block);
	if (compv > MAXULONG64 - block->hw_piv)
		return;
	tgpio_hw_write_compv_paused(block, compv + block->hw_piv);
}

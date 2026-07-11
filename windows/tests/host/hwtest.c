/* Host-side unit tests for the Windows TGPIO hardware layer. */
#include "ntddk.h"
#include "wdf.h"

struct reglog_entry reglog[REGLOG_MAX];
int reglog_count;
UCHAR fake_mmio[0x40];
unsigned long long fake_tsc;
unsigned long long fake_systime_100ns;

#include "../../driver/hw.c"
#include "../../driver/art.c"

#include <assert.h>

#define ART_HZ 38400000ull /* 38.4 MHz crystal */

static struct tgpio_win_device dev;
static struct tgpio_win_block *blk;

static void reset(void)
{
	memset(fake_mmio, 0, sizeof(fake_mmio));
	memset(reglog, 0, sizeof(reglog));
	reglog_count = 0;
	memset(&dev, 0, sizeof(dev));
	dev.art_frequency_hz = ART_HZ;
	dev.tsc_art_numerator = 2; /* TSC = 2 * ART */
	dev.tsc_art_denominator = 1;
	dev.tsc_adjust = 0;
	blk = &dev.block[0];
	memset(blk, 0, sizeof(*blk));
	blk->regs = (volatile UCHAR *)fake_mmio;
	blk->mmio_size = 0x38;
}

static unsigned long long reg64(ULONG lo_off)
{
	uint32_t lo, hi;
	memcpy(&lo, fake_mmio + lo_off, 4);
	memcpy(&hi, fake_mmio + lo_off + 4, 4);
	return ((unsigned long long)hi << 32) | lo;
}

static void set_reg64(ULONG lo_off, unsigned long long v)
{
	uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
	memcpy(fake_mmio + lo_off, &lo, 4);
	memcpy(fake_mmio + lo_off + 4, &hi, 4);
}

/* Index of the n-th write to `offset` in the log, -1 if absent. */
static int nth_write(ULONG offset, int n)
{
	int i, seen = 0;
	for (i = 0; i < reglog_count; i++)
		if (reglog[i].is_write && reglog[i].offset == offset)
			if (seen++ == n)
				return i;
	return -1;
}

static void test_art_math(void)
{
	reset();
	fake_tsc = 2000000ull; /* ART = 1000000 */
	assert(tgpio_art_now(&dev) == 1000000ull);
	assert(tgpio_art_ns_to_cycles(&dev, 1000000000ull) == ART_HZ);
	assert(tgpio_art_ns_to_cycles(&dev, 2000000ull) == 76800ull);
	printf("art math ok\n");
}

static void test_start_output_flop_low(void)
{
	unsigned long long half = ART_HZ / 2; /* 1 PPS */
	unsigned long long now_art = 10ull * ART_HZ;
	unsigned long long compv, piv;
	NTSTATUS st;
	int last_ctl_write;

	reset();
	fake_tsc = 2 * now_art;

	st = tgpio_hw_start_output(&dev, blk, half, half, 0);
	assert(st == STATUS_SUCCESS);

	compv = reg64(TGPIOCOMPV31_0);
	piv = reg64(TGPIOPIV31_0);
	assert(piv == half);
	/* Auto start: ~50 ms out, minus the 2-cycle HW delay, and past the
	 * 2 ms late-arm deadline. */
	assert(compv > now_art + tgpio_art_ns_to_cycles(&dev, 2000000ull));
	assert(compv == now_art + tgpio_art_ns_to_cycles(&dev, 50000000ull)
			- TGPIO_ART_HW_DELAY_CYCLES);

	/* CTL: final value EN|PM|EP_TOGGLE, and EN set only in the last
	 * CTL write of the sequence. */
	{
		uint32_t ctl;
		memcpy(&ctl, fake_mmio + TGPIOCTL, 4);
		assert(ctl == (TGPIOCTL_EN | TGPIOCTL_PM | TGPIOCTL_EP_TOGGLE));
	}
	last_ctl_write = -1;
	for (int i = 0; i < reglog_count; i++)
		if (reglog[i].is_write && reglog[i].offset == TGPIOCTL)
			last_ctl_write = i;
	assert(last_ctl_write >= 0 && reglog[last_ctl_write].value & TGPIOCTL_EN);
	for (int i = 0; i < last_ctl_write; i++)
		if (reglog[i].is_write && reglog[i].offset == TGPIOCTL)
			assert(!(reglog[i].value & TGPIOCTL_EN));

	/* PIV programmed before COMPV before the enabling CTL write. */
	assert(nth_write(TGPIOPIV31_0, 1) < nth_write(TGPIOCOMPV31_0, 1));
	assert(nth_write(TGPIOCOMPV31_0, 1) < last_ctl_write);

	assert(blk->hw_piv == half && blk->ckpt_compv == compv);
	assert(!blk->flop_high);
	printf("start output (flop low) ok\n");
}

static void test_start_output_flop_high_slot(void)
{
	unsigned long long half = ART_HZ / 2;
	unsigned long long now_art = 10ull * ART_HZ;
	unsigned long long requested = now_art + ART_HZ; /* 1 s out */
	unsigned long long compv;

	reset();
	fake_tsc = 2 * now_art;
	blk->flop_high = TRUE;

	assert(tgpio_hw_start_output(&dev, blk, half, half, requested) ==
	       STATUS_SUCCESS);
	compv = reg64(TGPIOCOMPV31_0);
	/* First toggle is the falling edge one low-time early. */
	assert(compv == requested - TGPIO_ART_HW_DELAY_CYCLES - half);
	/* Pending fall consumes the low time. */
	assert(reg64(TGPIOPIV31_0) == half);
	printf("start output (flop high slot) ok\n");
}

static void test_late_arm_guard(void)
{
	unsigned long long half = ART_HZ / 2;
	unsigned long long now_art = 10ull * ART_HZ;
	unsigned long long stale = now_art - 5 * ART_HZ; /* 5 s in the past */
	unsigned long long compv, dist;

	reset();
	fake_tsc = 2 * now_art;

	assert(tgpio_hw_start_output(&dev, blk, half, half, stale) ==
	       STATUS_SUCCESS);
	compv = reg64(TGPIOCOMPV31_0);
	assert(compv > now_art + tgpio_art_ns_to_cycles(&dev, 2000000ull) -
			       ART_HZ);
	/* Pushed by whole periods: still on the stale grid. */
	dist = compv - (stale - TGPIO_ART_HW_DELAY_CYCLES);
	assert(dist % (2 * half) == 0);
	printf("late-arm guard ok\n");
}

static void test_flop_fold_and_stop(void)
{
	unsigned long long half = ART_HZ / 2;
	unsigned long long now_art = 10ull * ART_HZ;
	unsigned long long compv;

	reset();
	fake_tsc = 2 * now_art;
	assert(tgpio_hw_start_output(&dev, blk, half, half, 0) ==
	       STATUS_SUCCESS);
	compv = reg64(TGPIOCOMPV31_0);

	/* Hardware generated 3 edges: COMPV advanced by 3*PIV. */
	set_reg64(TGPIOCOMPV31_0, compv + 3 * half);
	tgpio_hw_flop_fold(blk);
	assert(blk->flop_high); /* odd count flips */
	assert(blk->ckpt_compv == compv + 3 * half);

	/* Two more edges: even, no flip. */
	set_reg64(TGPIOCOMPV31_0, compv + 5 * half);
	tgpio_hw_flop_fold(blk);
	assert(blk->flop_high);

	/* Stop: EN drops first, then registers cleared, flop preserved. */
	reglog_count = 0;
	tgpio_hw_stop_output(blk);
	{
		int first_w = -1;
		for (int i = 0; i < reglog_count && first_w < 0; i++)
			if (reglog[i].is_write)
				first_w = i;
		assert(first_w >= 0 &&
		       reglog[first_w].offset == TGPIOCTL &&
		       !(reglog[first_w].value & TGPIOCTL_EN));
	}
	assert(reg64(TGPIOCOMPV31_0) == 0 && reg64(TGPIOPIV31_0) == 0);
	assert(blk->hw_piv == 0);
	assert(blk->flop_high); /* parity survives stop */
	printf("flop fold + stop ok\n");
}

static void test_invert(void)
{
	unsigned long long half = ART_HZ / 2;
	unsigned long long now_art = 10ull * ART_HZ;
	unsigned long long compv;
	int pm_clear, compv_w, pm_restore;

	reset();
	fake_tsc = 2 * now_art;
	assert(tgpio_hw_start_output(&dev, blk, half, half, 0) ==
	       STATUS_SUCCESS);
	compv = reg64(TGPIOCOMPV31_0);

	reglog_count = 0;
	tgpio_hw_invert_output(blk);
	assert(blk->flop_high);
	assert(reg64(TGPIOCOMPV31_0) == compv + half);

	/* The rewrite happens with PM cleared and EN kept. */
	pm_clear = -1; compv_w = -1; pm_restore = -1;
	for (int i = 0; i < reglog_count; i++) {
		if (!reglog[i].is_write)
			continue;
		if (reglog[i].offset == TGPIOCTL &&
		    !(reglog[i].value & TGPIOCTL_PM) && pm_clear < 0)
			pm_clear = i;
		if (reglog[i].offset == TGPIOCOMPV31_0)
			compv_w = i;
		if (reglog[i].offset == TGPIOCTL &&
		    (reglog[i].value & TGPIOCTL_PM))
			pm_restore = i;
	}
	assert(pm_clear >= 0 && compv_w > pm_clear && pm_restore > compv_w);
	assert(reglog[pm_clear].value & TGPIOCTL_EN);
	printf("invert ok\n");
}

static void test_input_sequence(void)
{
	uint32_t ctl;

	reset();
	tgpio_hw_set_input(blk, TRUE, TGPIOCTL_EP_FALLING);
	memcpy(&ctl, fake_mmio + TGPIOCTL, 4);
	assert(ctl == (TGPIOCTL_EN | TGPIOCTL_DIR | TGPIOCTL_EP_FALLING));

	tgpio_hw_set_input(blk, FALSE, 0);
	memcpy(&ctl, fake_mmio + TGPIOCTL, 4);
	assert(!(ctl & TGPIOCTL_EN));
	printf("input sequence ok\n");
}

static void test_capture_read(void)
{
	unsigned long long count, art;

	reset();
	set_reg64(TGPIOTCV31_0, 0x123456789abcdef0ull);
	set_reg64(TGPIOECCV31_0, 42);
	tgpio_hw_read_capture(blk, &count, &art);
	assert(count == 42 && art == 0x123456789abcdef0ull);
	printf("capture read ok\n");
}

int main(void)
{
	test_art_math();
	test_start_output_flop_low();
	test_start_output_flop_high_slot();
	test_late_arm_guard();
	test_flop_fold_and_stop();
	test_invert();
	test_input_sequence();
	test_capture_read();
	printf("all hw tests passed\n");
	return 0;
}

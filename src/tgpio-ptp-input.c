// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/*
 * Intel Time-Aware GPIO PTP input/output add-on driver.
 * Copyright (c) 2026 Ahmad Byagowi
 *
 * This out-of-tree driver is for systems where TGPIO hardware is present but
 * firmware does not expose the ACPI devices needed for enumeration. Each known
 * static MMIO block can be assigned to PTP external timestamp input, PTP
 * periodic output, or left off.
 */

#include <linux/bitfield.h>
#include <linux/clocksource_ids.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#ifdef CONFIG_X86
#include <asm/cpuid/api.h>
#endif

#define TGPIO_MAX_BLOCKS	2

#define TGPIOCTL		0x00
#define TGPIOCOMPV31_0		0x10
#define TGPIOCOMPV63_32		0x14
#define TGPIOPIV31_0		0x18
#define TGPIOPIV63_32		0x1c
#define TGPIOTCV31_0		0x20
#define TGPIOTCV63_32		0x24
#define TGPIOECCV31_0		0x28
#define TGPIOECCV63_32		0x2c
#define TGPIOEC31_0		0x30
#define TGPIOEC63_32		0x34

#define TGPIOCTL_EN		BIT(0)
#define TGPIOCTL_DIR		BIT(1)
#define TGPIOCTL_EP		GENMASK(3, 2)
#define TGPIOCTL_EP_RISING	(0u << 2)
#define TGPIOCTL_EP_FALLING	(1u << 2)
#define TGPIOCTL_EP_TOGGLE	(2u << 2)
#define TGPIOCTL_PM		BIT(4)

#define TGPIO_ART_HW_DELAY_CYCLES 2
#define TGPIO_OUTPUT_SAFE_TIME_NS (10 * NSEC_PER_MSEC)
#define TGPIO_CPUID_ART_LEAF	0x15

enum tgpio_mode {
	TGPIO_MODE_OFF,
	TGPIO_MODE_INPUT,
	TGPIO_MODE_OUTPUT,
};

static unsigned long addr0 = 0xFE001210;
static unsigned long addr1 = 0xFE001310;
static unsigned int mmio_size = 0x38;
static bool use_second = true;
static char mode0_param[16] = "input";
static char mode1_param[16] = "input";
static char edge0_param[16] = "both";
static char edge1_param[16] = "both";
static unsigned int poll_ms = 10;
static unsigned long art_frequency;
static unsigned int tsc_art_numerator;
static unsigned int tsc_art_denominator;
static bool hardware_timestamps = true;

module_param(addr0, ulong, 0444);
MODULE_PARM_DESC(addr0, "MMIO base for first TGPIO block");

module_param(addr1, ulong, 0444);
MODULE_PARM_DESC(addr1, "MMIO base for second TGPIO block");

module_param(mmio_size, uint, 0444);
MODULE_PARM_DESC(mmio_size, "MMIO size for each TGPIO block, default 0x38");

module_param(use_second, bool, 0444);
MODULE_PARM_DESC(use_second, "Enable the second TGPIO block");

module_param_string(mode0, mode0_param, sizeof(mode0_param), 0444);
MODULE_PARM_DESC(mode0, "Mode for block 0: input, output, or off");

module_param_string(mode1, mode1_param, sizeof(mode1_param), 0444);
MODULE_PARM_DESC(mode1, "Mode for block 1: input, output, or off");

module_param_string(edge0, edge0_param, sizeof(edge0_param), 0444);
MODULE_PARM_DESC(edge0, "Default input edge for block 0: rising, falling, or both");

module_param_string(edge1, edge1_param, sizeof(edge1_param), 0444);
MODULE_PARM_DESC(edge1, "Default input edge for block 1: rising, falling, or both");

module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "Polling interval for captured input events");

module_param(art_frequency, ulong, 0644);
MODULE_PARM_DESC(art_frequency,
		 "ART frequency in Hz used to convert capture cycles to "
		 "nanoseconds; 0 auto-detects from CPUID leaf 0x15");

module_param(tsc_art_numerator, uint, 0444);
MODULE_PARM_DESC(tsc_art_numerator,
		 "Detected CPUID leaf 0x15 TSC/ART numerator; 0 unknown");

module_param(tsc_art_denominator, uint, 0444);
MODULE_PARM_DESC(tsc_art_denominator,
		 "Detected CPUID leaf 0x15 TSC/ART denominator; 0 unknown");

module_param(hardware_timestamps, bool, 0644);
MODULE_PARM_DESC(hardware_timestamps, "Use hardware capture time instead of poll time");

struct tgpio_block {
	unsigned int index;
	unsigned long addr;
	enum tgpio_mode mode;
	void __iomem *base;
	bool enabled;
	u32 input_edge_bits;
	u64 last_count;
	struct hrtimer output_timer;
	spinlock_t output_lock;
	bool output_enabled;
	u64 output_half_period_ns;
	ktime_t output_next_edge;
};

struct tgpio_state {
	struct ptp_clock_info info;
	struct ptp_clock *clock;
	struct ptp_pin_desc pin_config[TGPIO_MAX_BLOCKS];
	struct delayed_work poll_work;
	struct mutex lock;
	unsigned int n_ptp_pins;
	struct tgpio_block blocks[TGPIO_MAX_BLOCKS];
};

static struct tgpio_state *tgpio;

static const char *tgpio_mode_name(enum tgpio_mode mode)
{
	switch (mode) {
	case TGPIO_MODE_INPUT:
		return "input";
	case TGPIO_MODE_OUTPUT:
		return "output";
	case TGPIO_MODE_OFF:
	default:
		return "off";
	}
}

static int tgpio_parse_mode(const char *value, enum tgpio_mode *mode)
{
	if (sysfs_streq(value, "input") || sysfs_streq(value, "in")) {
		*mode = TGPIO_MODE_INPUT;
		return 0;
	}

	if (sysfs_streq(value, "output") || sysfs_streq(value, "out") ||
	    sysfs_streq(value, "pps")) {
		*mode = TGPIO_MODE_OUTPUT;
		return 0;
	}

	if (sysfs_streq(value, "off") || sysfs_streq(value, "none") ||
	    sysfs_streq(value, "disabled")) {
		*mode = TGPIO_MODE_OFF;
		return 0;
	}

	return -EINVAL;
}

static int tgpio_parse_edge(const char *value, u32 *edge_bits)
{
	if (sysfs_streq(value, "rising") || sysfs_streq(value, "rise")) {
		*edge_bits = TGPIOCTL_EP_RISING;
		return 0;
	}

	if (sysfs_streq(value, "falling") || sysfs_streq(value, "fall")) {
		*edge_bits = TGPIOCTL_EP_FALLING;
		return 0;
	}

	if (sysfs_streq(value, "both") || sysfs_streq(value, "toggle") ||
	    sysfs_streq(value, "all")) {
		*edge_bits = TGPIOCTL_EP_TOGGLE;
		return 0;
	}

	return -EINVAL;
}

static inline u32 tgpio_readl(struct tgpio_block *block, u32 offset)
{
	return readl(block->base + offset);
}

static inline void tgpio_writel(struct tgpio_block *block, u32 offset,
				u32 value)
{
	writel(value, block->base + offset);
}

static inline void tgpio_write_compv(struct tgpio_block *block, u64 value)
{
	tgpio_writel(block, TGPIOCOMPV63_32, upper_32_bits(value));
	tgpio_writel(block, TGPIOCOMPV31_0, lower_32_bits(value));
}

static bool tgpio_detect_cpuid_art_frequency(unsigned long *frequency)
{
#ifdef CONFIG_X86
	u32 max_leaf;
	u32 eax;
	u32 ebx;
	u32 ecx;
	u32 edx;

	cpuid(0, &max_leaf, &ebx, &ecx, &edx);
	if (max_leaf < TGPIO_CPUID_ART_LEAF)
		return false;

	cpuid_count(TGPIO_CPUID_ART_LEAF, 0, &eax, &ebx, &ecx, &edx);
	if (!eax || !ebx)
		return false;

	tsc_art_numerator = ebx;
	tsc_art_denominator = eax;
	if (!ecx)
		return false;

	*frequency = ecx;
	return true;
#else
	(void)frequency;
	return false;
#endif
}

static int tgpio_resolve_art_frequency(void)
{
	unsigned long detected;
	bool cpuid_art_frequency;

	cpuid_art_frequency = tgpio_detect_cpuid_art_frequency(&detected);

	if (art_frequency) {
		pr_info("using manual ART frequency %lu Hz\n",
			art_frequency);
		if (tsc_art_numerator && tsc_art_denominator)
			pr_info("detected CPUID leaf %#x TSC/ART ratio %u/%u\n",
				TGPIO_CPUID_ART_LEAF, tsc_art_numerator,
				tsc_art_denominator);
		return 0;
	}

	if (cpuid_art_frequency) {
		art_frequency = detected;
		pr_info("auto-detected ART frequency %lu Hz from CPUID leaf %#x; TSC/ART ratio %u/%u\n",
			art_frequency, TGPIO_CPUID_ART_LEAF,
			tsc_art_numerator, tsc_art_denominator);
		return 0;
	}

	if (tsc_art_numerator && tsc_art_denominator) {
		pr_err("CPUID leaf %#x reported TSC/ART ratio %u/%u but no ART frequency; set art_frequency=<Hz>\n",
		       TGPIO_CPUID_ART_LEAF, tsc_art_numerator,
		       tsc_art_denominator);
	} else {
		pr_err("could not auto-detect ART frequency; set art_frequency=<Hz>\n");
	}
	return -ENODEV;
}

static int tgpio_check_addr(unsigned long addr)
{
	if (!addr || !mmio_size)
		return -EINVAL;

	if (addr + mmio_size - 1 < addr)
		return -EOVERFLOW;

	return 0;
}

static ktime_t tgpio_output_lead_time(u64 half_period_ns)
{
	u64 lead = min_t(u64, TGPIO_OUTPUT_SAFE_TIME_NS,
			 div64_u64(half_period_ns, 4));

	if (!lead)
		lead = 1;

	return ns_to_ktime(lead);
}

static u64 tgpio_art_to_ns(u64 art)
{
	u64 seconds;
	u64 rem;

	if (!art_frequency)
		return 0;

	seconds = div64_u64_rem(art, art_frequency, &rem);
	return seconds * NSEC_PER_SEC + div64_ul(rem * NSEC_PER_SEC,
						 art_frequency);
}

static void tgpio_read_capture(struct tgpio_block *block, u64 *event_count,
			       u64 *art_cycles)
{
	u32 tcv_lo;
	u32 tcv_hi;
	u32 count_lo;
	u32 count_hi;

	/* Reading TCV low latches the capture timestamp and event count. */
	tcv_lo = tgpio_readl(block, TGPIOTCV31_0);
	count_hi = tgpio_readl(block, TGPIOECCV63_32);
	count_lo = tgpio_readl(block, TGPIOECCV31_0);
	tcv_hi = tgpio_readl(block, TGPIOTCV63_32);

	*event_count = ((u64)count_hi << 32) | count_lo;
	*art_cycles = ((u64)tcv_hi << 32) | tcv_lo;
}

static bool tgpio_block_supports_func(struct tgpio_state *state,
				      unsigned int block_index,
				      enum ptp_pin_function func)
{
	if (block_index >= state->n_ptp_pins)
		return false;

	switch (func) {
	case PTP_PF_EXTTS:
		return state->blocks[block_index].mode == TGPIO_MODE_INPUT;
	case PTP_PF_PEROUT:
		return state->blocks[block_index].mode == TGPIO_MODE_OUTPUT;
	default:
		return false;
	}
}

static int tgpio_find_block_for_channel(struct tgpio_state *state,
					enum ptp_pin_function func,
					unsigned int channel)
{
	unsigned int i;

	if (channel >= state->n_ptp_pins)
		return -EINVAL;

	for (i = 0; i < state->n_ptp_pins; i++) {
		if (!tgpio_block_supports_func(state, i, func))
			continue;

		if (state->pin_config[i].func == func &&
		    state->pin_config[i].chan == channel)
			return i;
	}

	if (tgpio_block_supports_func(state, channel, func))
		return channel;

	return -EOPNOTSUPP;
}

static unsigned int tgpio_channel_for_block(struct tgpio_state *state,
					    unsigned int block_index,
					    enum ptp_pin_function func)
{
	if (block_index < state->n_ptp_pins &&
	    state->pin_config[block_index].func == func)
		return state->pin_config[block_index].chan;

	return block_index;
}

static void tgpio_emit_event(struct tgpio_state *state, unsigned int index,
			     u64 art_cycles)
{
	struct ptp_clock_event event = {
		.type = PTP_CLOCK_EXTTS,
		.index = index,
	};

	if (!state->clock)
		return;

	if (hardware_timestamps)
		event.timestamp = tgpio_art_to_ns(art_cycles);
	else
		event.timestamp = ktime_get_real_ns();

	ptp_clock_event(state->clock, &event);
}

static void tgpio_poll_work(struct work_struct *work)
{
	struct tgpio_state *state =
		container_of(to_delayed_work(work), struct tgpio_state,
			     poll_work);
	bool enabled = false;
	unsigned int i;

	mutex_lock(&state->lock);
	for (i = 0; i < ARRAY_SIZE(state->blocks); i++) {
		struct tgpio_block *block = &state->blocks[i];
		u64 count;
		u64 art;

		if (block->mode != TGPIO_MODE_INPUT || !block->enabled)
			continue;

		enabled = true;
		tgpio_read_capture(block, &count, &art);

		if (count != block->last_count) {
			unsigned int channel;

			block->last_count = count;
			channel = tgpio_channel_for_block(state, i,
							  PTP_PF_EXTTS);
			tgpio_emit_event(state, channel, art);
		}
	}
	mutex_unlock(&state->lock);

	if (enabled)
		schedule_delayed_work(&state->poll_work,
				      msecs_to_jiffies(poll_ms ? poll_ms : 1));
}

static u32 tgpio_edge_bits(struct tgpio_block *block, unsigned int flags)
{
	bool rising = flags & PTP_RISING_EDGE;
	bool falling = flags & PTP_FALLING_EDGE;

	if (!rising && !falling)
		return block->input_edge_bits;

	if (rising && !falling)
		return TGPIOCTL_EP_RISING;
	if (!rising && falling)
		return TGPIOCTL_EP_FALLING;

	return TGPIOCTL_EP_TOGGLE;
}

static int tgpio_config_input(struct tgpio_state *state, unsigned int channel,
			      unsigned int flags, int on)
{
	struct tgpio_block *block;
	int block_index;
	u64 count;
	u64 art;
	u32 ctrl;

	block_index = tgpio_find_block_for_channel(state, PTP_PF_EXTTS,
						   channel);
	if (block_index < 0)
		return block_index;

	block = &state->blocks[block_index];
	if (block->mode != TGPIO_MODE_INPUT || !block->base)
		return -EOPNOTSUPP;

	ctrl = tgpio_readl(block, TGPIOCTL);
	ctrl &= ~TGPIOCTL_EN;
	tgpio_writel(block, TGPIOCTL, ctrl);

	if (on) {
		ctrl &= ~TGPIOCTL_EP;
		ctrl |= tgpio_edge_bits(block, flags);
		ctrl |= TGPIOCTL_DIR;
		tgpio_writel(block, TGPIOCTL, ctrl);

		tgpio_read_capture(block, &count, &art);
		block->last_count = count;
		block->enabled = true;

		ctrl |= TGPIOCTL_EN;
		tgpio_writel(block, TGPIOCTL, ctrl);
		schedule_delayed_work(&state->poll_work,
				      msecs_to_jiffies(poll_ms ? poll_ms : 1));
	} else {
		block->enabled = false;
		tgpio_writel(block, TGPIOCTL, ctrl);
	}

	return 0;
}

static int tgpio_ptp_time_to_ns(const struct ptp_clock_time *time, u64 *ns)
{
	if (time->sec < 0 || time->nsec >= NSEC_PER_SEC)
		return -EINVAL;

	if (time->sec > div64_u64(U64_MAX, NSEC_PER_SEC))
		return -ERANGE;

	*ns = (u64)time->sec * NSEC_PER_SEC + time->nsec;
	return 0;
}

static ktime_t tgpio_ptp_time_to_ktime(const struct ptp_clock_time *time)
{
	return ktime_set(time->sec, time->nsec);
}

static bool tgpio_program_output_edge(struct tgpio_block *block,
				      ktime_t edge_time)
{
	u64 art;

	if (!ktime_real_to_base_clock(edge_time, CSID_X86_ART, &art))
		return false;

	if (art > TGPIO_ART_HW_DELAY_CYCLES)
		art -= TGPIO_ART_HW_DELAY_CYCLES;

	tgpio_write_compv(block, art);
	return true;
}

static void tgpio_disable_output_hw(struct tgpio_block *block)
{
	u32 ctrl;

	ctrl = tgpio_readl(block, TGPIOCTL);
	tgpio_write_compv(block, 0);
	ctrl &= ~TGPIOCTL_EN;
	tgpio_writel(block, TGPIOCTL, ctrl);
	block->output_enabled = false;
}

static enum hrtimer_restart tgpio_output_timer(struct hrtimer *timer)
{
	struct tgpio_block *block =
		container_of(timer, struct tgpio_block, output_timer);
	ktime_t now;
	ktime_t next_edge;
	unsigned long flags;

	spin_lock_irqsave(&block->output_lock, flags);
	if (!block->output_enabled) {
		spin_unlock_irqrestore(&block->output_lock, flags);
		return HRTIMER_NORESTART;
	}

	next_edge = ktime_add_ns(block->output_next_edge,
				 block->output_half_period_ns);
	if (!tgpio_program_output_edge(block, next_edge)) {
		tgpio_disable_output_hw(block);
		spin_unlock_irqrestore(&block->output_lock, flags);
		return HRTIMER_NORESTART;
	}

	block->output_next_edge = next_edge;
	now = ktime_get_real();
	hrtimer_forward(timer, now, ns_to_ktime(block->output_half_period_ns));
	spin_unlock_irqrestore(&block->output_lock, flags);

	return HRTIMER_RESTART;
}

static void tgpio_disable_output(struct tgpio_block *block)
{
	unsigned long flags;

	hrtimer_cancel(&block->output_timer);

	spin_lock_irqsave(&block->output_lock, flags);
	if (block->base)
		tgpio_disable_output_hw(block);
	spin_unlock_irqrestore(&block->output_lock, flags);
}

static int tgpio_config_output(struct tgpio_state *state,
			       const struct ptp_perout_request *perout,
			       int on)
{
	struct tgpio_block *block;
	int block_index;
	ktime_t first_edge;
	ktime_t min_first_edge;
	ktime_t timer_start;
	ktime_t lead;
	unsigned long irqflags;
	u64 period_ns;
	u64 half_period_ns;
	int ret;
	u32 ctrl;

	block_index = tgpio_find_block_for_channel(state, PTP_PF_PEROUT,
						   perout->index);
	if (block_index < 0)
		return block_index;

	block = &state->blocks[block_index];
	if (block->mode != TGPIO_MODE_OUTPUT || !block->base)
		return -EOPNOTSUPP;

	if (!on) {
		tgpio_disable_output(block);
		return 0;
	}

	if (perout->flags)
		return -EOPNOTSUPP;

	ret = tgpio_ptp_time_to_ns(&perout->period, &period_ns);
	if (ret)
		return ret;

	half_period_ns = div64_u64(period_ns, 2);
	if (!half_period_ns)
		return -EINVAL;

	if (!timekeeping_clocksource_has_base(CSID_X86_ART))
		return -ENODEV;

	first_edge = tgpio_ptp_time_to_ktime(&perout->start);
	lead = tgpio_output_lead_time(half_period_ns);
	min_first_edge = ktime_add(ktime_get_real(), lead);
	if (ktime_before(first_edge, min_first_edge))
		first_edge = min_first_edge;

	tgpio_disable_output(block);

	spin_lock_irqsave(&block->output_lock, irqflags);

	ctrl = tgpio_readl(block, TGPIOCTL);
	ctrl &= ~TGPIOCTL_EN;
	tgpio_writel(block, TGPIOCTL, ctrl);
	tgpio_write_compv(block, 0);

	ctrl &= ~(TGPIOCTL_DIR | TGPIOCTL_EP | TGPIOCTL_PM);
	ctrl |= TGPIOCTL_EP_TOGGLE;
	tgpio_writel(block, TGPIOCTL, ctrl);

	if (!tgpio_program_output_edge(block, first_edge)) {
		tgpio_disable_output_hw(block);
		spin_unlock_irqrestore(&block->output_lock, irqflags);
		return -ENODEV;
	}

	block->output_half_period_ns = half_period_ns;
	block->output_next_edge = first_edge;
	block->output_enabled = true;

	ctrl |= TGPIOCTL_EN;
	tgpio_writel(block, TGPIOCTL, ctrl);

	spin_unlock_irqrestore(&block->output_lock, irqflags);

	timer_start = ktime_sub(ktime_add_ns(first_edge, half_period_ns),
				lead);
	if (ktime_before(timer_start, ktime_get_real()))
		timer_start = ktime_get_real();

	hrtimer_start(&block->output_timer, timer_start, HRTIMER_MODE_ABS);
	return 0;
}

static int tgpio_ptp_enable(struct ptp_clock_info *ptp,
			    struct ptp_clock_request *req, int on)
{
	struct tgpio_state *state =
		container_of(ptp, struct tgpio_state, info);
	int ret;

	mutex_lock(&state->lock);
	switch (req->type) {
	case PTP_CLK_REQ_EXTTS:
		ret = tgpio_config_input(state, req->extts.index,
					 req->extts.flags, on);
		break;
	case PTP_CLK_REQ_PEROUT:
		ret = tgpio_config_output(state, &req->perout, on);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&state->lock);

	return ret;
}

static int tgpio_ptp_gettime64(struct ptp_clock_info *ptp,
			       struct timespec64 *ts)
{
	ktime_get_real_ts64(ts);
	return 0;
}

static int tgpio_ptp_settime64(struct ptp_clock_info *ptp,
			       const struct timespec64 *ts)
{
	return -EOPNOTSUPP;
}

static int tgpio_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	return -EOPNOTSUPP;
}

static int tgpio_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	return -EOPNOTSUPP;
}

static int tgpio_ptp_verify(struct ptp_clock_info *ptp, unsigned int pin,
			    enum ptp_pin_function func, unsigned int chan)
{
	struct tgpio_state *state =
		container_of(ptp, struct tgpio_state, info);

	if (pin >= state->n_ptp_pins)
		return -EINVAL;

	if (func == PTP_PF_NONE)
		return 0;

	if (chan >= state->n_ptp_pins)
		return -EINVAL;

	switch (func) {
	case PTP_PF_EXTTS:
		if (!tgpio_block_supports_func(state, pin, PTP_PF_EXTTS))
			return -EOPNOTSUPP;
		return 0;
	case PTP_PF_PEROUT:
		if (!tgpio_block_supports_func(state, pin, PTP_PF_PEROUT))
			return -EOPNOTSUPP;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static void tgpio_disable_inputs(struct tgpio_state *state)
{
	unsigned int i;

	cancel_delayed_work_sync(&state->poll_work);

	mutex_lock(&state->lock);
	for (i = 0; i < ARRAY_SIZE(state->blocks); i++) {
		struct tgpio_block *block = &state->blocks[i];
		u32 ctrl;

		if (block->mode != TGPIO_MODE_INPUT || !block->base)
			continue;

		ctrl = tgpio_readl(block, TGPIOCTL);
		ctrl &= ~TGPIOCTL_EN;
		tgpio_writel(block, TGPIOCTL, ctrl);
		block->enabled = false;
	}
	mutex_unlock(&state->lock);
}

static void tgpio_disable_outputs(struct tgpio_state *state)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(state->blocks); i++) {
		struct tgpio_block *block = &state->blocks[i];

		if (block->mode == TGPIO_MODE_OUTPUT)
			tgpio_disable_output(block);
	}
}

static void tgpio_disable_blocks(struct tgpio_state *state)
{
	tgpio_disable_inputs(state);
	tgpio_disable_outputs(state);
}

static void tgpio_unmap_blocks(struct tgpio_state *state)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(state->blocks); i++) {
		struct tgpio_block *block = &state->blocks[i];

		if (!block->base)
			continue;

		iounmap(block->base);
		block->base = NULL;
	}
}

static int tgpio_map_block(struct tgpio_state *state, unsigned int index)
{
	struct tgpio_block *block = &state->blocks[index];
	int ret;

	ret = tgpio_check_addr(block->addr);
	if (ret)
		return ret;

	block->base = ioremap(block->addr, mmio_size);
	if (!block->base)
		return -ENOMEM;

	state->n_ptp_pins = max(state->n_ptp_pins, index + 1);
	pr_info("block %u %s at %#lx-%#lx\n", index,
		tgpio_mode_name(block->mode), block->addr,
		block->addr + mmio_size - 1);
	return 0;
}

static int tgpio_configure_blocks(struct tgpio_state *state)
{
	const char *modes[TGPIO_MAX_BLOCKS] = { mode0_param, mode1_param };
	const char *edges[TGPIO_MAX_BLOCKS] = { edge0_param, edge1_param };
	unsigned long addrs[TGPIO_MAX_BLOCKS] = { addr0, addr1 };
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(state->blocks); i++) {
		struct tgpio_block *block = &state->blocks[i];

		block->index = i;
		block->addr = addrs[i];
		spin_lock_init(&block->output_lock);
		hrtimer_setup(&block->output_timer, tgpio_output_timer,
			      CLOCK_REALTIME, HRTIMER_MODE_ABS);

		ret = tgpio_parse_mode(modes[i], &block->mode);
		if (ret) {
			pr_err("invalid mode%u=%s; use input, output, or off\n",
			       i, modes[i]);
			return ret;
		}

		ret = tgpio_parse_edge(edges[i], &block->input_edge_bits);
		if (ret) {
			pr_err("invalid edge%u=%s; use rising, falling, or both\n",
			       i, edges[i]);
			return ret;
		}
	}

	if (!use_second)
		state->blocks[1].mode = TGPIO_MODE_OFF;

	for (i = 0; i < ARRAY_SIZE(state->blocks); i++) {
		struct tgpio_block *block = &state->blocks[i];

		switch (block->mode) {
		case TGPIO_MODE_INPUT:
		case TGPIO_MODE_OUTPUT:
			ret = tgpio_map_block(state, i);
			if (ret)
				return ret;
			break;
		case TGPIO_MODE_OFF:
			pr_info("block %u off\n", i);
			break;
		}
	}

	return 0;
}

static bool tgpio_needs_art_frequency(struct tgpio_state *state)
{
	unsigned int i;

	if (!hardware_timestamps)
		return false;

	for (i = 0; i < ARRAY_SIZE(state->blocks); i++) {
		if (state->blocks[i].mode == TGPIO_MODE_INPUT)
			return true;
	}

	return false;
}

static void tgpio_setup_pin_descs(struct tgpio_state *state)
{
	unsigned int i;

	for (i = 0; i < state->n_ptp_pins; i++) {
		snprintf(state->pin_config[i].name,
			 sizeof(state->pin_config[i].name), "tgpio%u-%s",
			 i, tgpio_mode_name(state->blocks[i].mode));
		state->pin_config[i].index = i;
		state->pin_config[i].func = PTP_PF_NONE;
		state->pin_config[i].chan = i;
	}
}

static int tgpio_register_ptp_clock(struct tgpio_state *state)
{
	int ret;

	if (!state->n_ptp_pins)
		return 0;

	tgpio_setup_pin_descs(state);

	state->info.owner = THIS_MODULE;
	snprintf(state->info.name, sizeof(state->info.name),
		 "Intel TGPIO");
	state->info.max_adj = 0;
	state->info.n_pins = state->n_ptp_pins;
	state->info.n_ext_ts = state->n_ptp_pins;
	state->info.n_per_out = state->n_ptp_pins;
	state->info.supported_perout_flags = 0;
	state->info.pin_config = state->pin_config;
	state->info.adjfine = tgpio_ptp_adjfine;
	state->info.adjtime = tgpio_ptp_adjtime;
	state->info.gettime64 = tgpio_ptp_gettime64;
	state->info.settime64 = tgpio_ptp_settime64;
	state->info.enable = tgpio_ptp_enable;
	state->info.verify = tgpio_ptp_verify;

	state->clock = ptp_clock_register(&state->info, NULL);
	if (IS_ERR(state->clock)) {
		ret = PTR_ERR(state->clock);
		state->clock = NULL;
		return ret;
	}

	pr_info("registered PTP clock with %u pin slot(s)\n",
		state->n_ptp_pins);
	return 0;
}

static int __init tgpio_input_init(void)
{
	int ret;

	if (!mmio_size)
		return -EINVAL;

	tgpio = kzalloc(sizeof(*tgpio), GFP_KERNEL);
	if (!tgpio)
		return -ENOMEM;

	mutex_init(&tgpio->lock);
	INIT_DELAYED_WORK(&tgpio->poll_work, tgpio_poll_work);

	ret = tgpio_configure_blocks(tgpio);
	if (ret)
		goto err_cleanup;

	if (tgpio_needs_art_frequency(tgpio)) {
		ret = tgpio_resolve_art_frequency();
		if (ret)
			goto err_cleanup;
	} else if (!art_frequency) {
		pr_info("ART frequency not needed for this configuration\n");
	}

	ret = tgpio_register_ptp_clock(tgpio);
	if (ret)
		goto err_cleanup;

	pr_info("loaded with mode0=%s mode1=%s\n",
		tgpio_mode_name(tgpio->blocks[0].mode),
		tgpio_mode_name(tgpio->blocks[1].mode));
	return 0;

err_cleanup:
	tgpio_disable_blocks(tgpio);
	if (tgpio->clock)
		ptp_clock_unregister(tgpio->clock);
	tgpio_unmap_blocks(tgpio);
	kfree(tgpio);
	tgpio = NULL;
	return ret;
}

static void __exit tgpio_input_exit(void)
{
	if (!tgpio)
		return;

	tgpio_disable_blocks(tgpio);

	if (tgpio->clock)
		ptp_clock_unregister(tgpio->clock);

	tgpio_unmap_blocks(tgpio);

	kfree(tgpio);
	tgpio = NULL;
}

module_init(tgpio_input_init);
module_exit(tgpio_input_exit);

MODULE_AUTHOR("Ahmad Byagowi");
MODULE_DESCRIPTION("Intel TGPIO per-block PTP input/output add-on driver");
MODULE_LICENSE("Dual BSD/GPL");

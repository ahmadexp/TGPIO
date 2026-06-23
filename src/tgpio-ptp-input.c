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
#include <linux/limits.h>
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
#define TGPIO_PHC_MAX_ADJ_PPB	100000000L

enum tgpio_mode {
	TGPIO_MODE_OFF,
	TGPIO_MODE_INPUT,
	TGPIO_MODE_OUTPUT,
};

enum tgpio_timestamp_mode {
	TGPIO_TIMESTAMP_REALTIME,
	TGPIO_TIMESTAMP_ART,
};

enum tgpio_clock_mode {
	TGPIO_CLOCK_REALTIME,
	TGPIO_CLOCK_PHC,
};

enum tgpio_output_phase {
	TGPIO_OUTPUT_FIRST_RISING,
	TGPIO_OUTPUT_TOGGLE,
};

enum tgpio_output_polarity {
	TGPIO_OUTPUT_NORMAL,
	TGPIO_OUTPUT_INVERTED,
};

static unsigned long addr0 = 0xFE001210;
static unsigned long addr1 = 0xFE001310;
static unsigned int mmio_size = 0x38;
static bool use_second = true;
static char mode0_param[16] = "input";
static char mode1_param[16] = "input";
static char edge0_param[16] = "rising";
static char edge1_param[16] = "rising";
static char clock_mode_param[16] = "phc";
static char timestamp_mode_param[16] = "realtime";
static char output_polarity_param[16] = "normal";
static unsigned int poll_ms = 10;
static unsigned long art_frequency;
static unsigned int tsc_art_numerator;
static unsigned int tsc_art_denominator;
static enum tgpio_clock_mode clock_mode;
static enum tgpio_timestamp_mode timestamp_mode;
static enum tgpio_output_polarity output_polarity;
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

module_param_string(clock_mode, clock_mode_param, sizeof(clock_mode_param), 0444);
MODULE_PARM_DESC(clock_mode,
		 "PTP clock mode: realtime or phc");

module_param_string(timestamp_mode, timestamp_mode_param,
		    sizeof(timestamp_mode_param), 0444);
MODULE_PARM_DESC(timestamp_mode,
		 "Hardware timestamp mode: realtime or art");

module_param_string(output_polarity, output_polarity_param,
		    sizeof(output_polarity_param), 0444);
MODULE_PARM_DESC(output_polarity,
		 "Periodic output polarity: normal or inverted");

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

struct tgpio_phc_clock {
	spinlock_t lock;
	u64 anchor_art;
	s64 anchor_ns;
	long scaled_ppm;
};

struct tgpio_block {
	struct tgpio_state *state;
	unsigned int index;
	unsigned long addr;
	enum tgpio_mode mode;
	void __iomem *base;
	bool enabled;
	u32 input_edge_bits;
	u64 last_count;
	struct system_time_snapshot timestamp_history;
	bool timestamp_history_valid;
	struct hrtimer output_timer;
	spinlock_t output_lock;
	bool output_enabled;
	enum tgpio_output_phase output_phase;
	u64 output_half_period_ns;
	ktime_t output_next_edge;
};

struct tgpio_state {
	struct ptp_clock_info info;
	struct ptp_clock *clock;
	struct ptp_pin_desc pin_config[TGPIO_MAX_BLOCKS];
	struct tgpio_phc_clock phc;
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

static const char *tgpio_clock_mode_name(enum tgpio_clock_mode mode)
{
	switch (mode) {
	case TGPIO_CLOCK_REALTIME:
		return "realtime";
	case TGPIO_CLOCK_PHC:
		return "phc";
	default:
		return "unknown";
	}
}

static int tgpio_parse_clock_mode(const char *value,
				  enum tgpio_clock_mode *mode)
{
	if (sysfs_streq(value, "realtime") ||
	    sysfs_streq(value, "clock_realtime") ||
	    sysfs_streq(value, "real")) {
		*mode = TGPIO_CLOCK_REALTIME;
		return 0;
	}

	if (sysfs_streq(value, "phc") ||
	    sysfs_streq(value, "adjustable") ||
	    sysfs_streq(value, "adjusted")) {
		*mode = TGPIO_CLOCK_PHC;
		return 0;
	}

	return -EINVAL;
}

static const char *tgpio_timestamp_mode_name(enum tgpio_timestamp_mode mode)
{
	switch (mode) {
	case TGPIO_TIMESTAMP_REALTIME:
		return "realtime";
	case TGPIO_TIMESTAMP_ART:
		return "art";
	default:
		return "unknown";
	}
}

static int tgpio_parse_timestamp_mode(const char *value,
				      enum tgpio_timestamp_mode *mode)
{
	if (sysfs_streq(value, "realtime") ||
	    sysfs_streq(value, "clock_realtime") ||
	    sysfs_streq(value, "real")) {
		*mode = TGPIO_TIMESTAMP_REALTIME;
		return 0;
	}

	if (sysfs_streq(value, "art") || sysfs_streq(value, "raw")) {
		*mode = TGPIO_TIMESTAMP_ART;
		return 0;
	}

	return -EINVAL;
}

static const char *tgpio_output_polarity_name(enum tgpio_output_polarity polarity)
{
	switch (polarity) {
	case TGPIO_OUTPUT_NORMAL:
		return "normal";
	case TGPIO_OUTPUT_INVERTED:
		return "inverted";
	default:
		return "unknown";
	}
}

static int tgpio_parse_output_polarity(const char *value,
				       enum tgpio_output_polarity *polarity)
{
	if (sysfs_streq(value, "normal") ||
	    sysfs_streq(value, "active_high")) {
		*polarity = TGPIO_OUTPUT_NORMAL;
		return 0;
	}

	if (sysfs_streq(value, "inverted") ||
	    sysfs_streq(value, "invert") ||
	    sysfs_streq(value, "active_low")) {
		*polarity = TGPIO_OUTPUT_INVERTED;
		return 0;
	}

	return -EINVAL;
}

static u32 tgpio_output_edge_bits(bool rising)
{
	/*
	 * TGPIO output compare polarity is opposite the input-edge names on
	 * the confirmed platform; keep logical rising/falling decisions here.
	 */
	if (output_polarity == TGPIO_OUTPUT_INVERTED)
		return rising ? TGPIOCTL_EP_RISING : TGPIOCTL_EP_FALLING;

	return rising ? TGPIOCTL_EP_FALLING : TGPIOCTL_EP_RISING;
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
		pr_err("CPUID leaf %#x reported TSC/ART ratio %u/%u but no "
		       "ART frequency; set art_frequency=<Hz>\n",
		       TGPIO_CPUID_ART_LEAF, tsc_art_numerator,
		       tsc_art_denominator);
	} else {
		pr_err("could not auto-detect ART frequency; set art_frequency=<Hz>\n");
	}
	return -ENODEV;
}

static void tgpio_probe_art_parameters_for_status(void)
{
	unsigned long detected;

	if (tgpio_detect_cpuid_art_frequency(&detected)) {
		if (!art_frequency)
			art_frequency = detected;
		pr_info("detected CPUID leaf %#x ART frequency %lu Hz; "
			"TSC/ART ratio %u/%u\n",
			TGPIO_CPUID_ART_LEAF, detected,
			tsc_art_numerator, tsc_art_denominator);
	} else if (tsc_art_numerator && tsc_art_denominator) {
		pr_info("detected CPUID leaf %#x TSC/ART ratio %u/%u; "
			"ART frequency not reported\n",
			TGPIO_CPUID_ART_LEAF, tsc_art_numerator,
			tsc_art_denominator);
	}
}

static int tgpio_check_addr(unsigned long addr)
{
	if (!addr || !mmio_size)
		return -EINVAL;

	if (addr + mmio_size - 1 < addr)
		return -EOVERFLOW;

	return 0;
}

static u64 tgpio_output_lead_time_ns(u64 half_period_ns)
{
	u64 lead = min_t(u64, TGPIO_OUTPUT_SAFE_TIME_NS,
			 div64_u64(half_period_ns, 4));

	if (!lead)
		lead = 1;

	return lead;
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

static u64 tgpio_abs_s64(s64 value)
{
	if (value < 0)
		return (u64)(-(value + 1)) + 1;

	return value;
}

static bool tgpio_add_s64_overflow(s64 lhs, s64 rhs, s64 *result)
{
	if (rhs > 0 && lhs > S64_MAX - rhs)
		return true;

	if (rhs < 0 && lhs < S64_MIN - rhs)
		return true;

	*result = lhs + rhs;
	return false;
}

static bool tgpio_u64_to_s64(u64 value, s64 *result)
{
	if (value > S64_MAX)
		return false;

	*result = value;
	return true;
}

static bool tgpio_art_delta_to_ns(s64 cycles, s64 *ns)
{
	u64 abs_cycles = tgpio_abs_s64(cycles);
	u64 seconds;
	u64 rem;
	u64 abs_ns;

	if (!art_frequency)
		return false;

	seconds = div64_u64_rem(abs_cycles, art_frequency, &rem);
	if (seconds > div64_u64(S64_MAX, NSEC_PER_SEC))
		return false;

	abs_ns = seconds * NSEC_PER_SEC +
		 div64_ul(rem * NSEC_PER_SEC, art_frequency);
	if (abs_ns > S64_MAX)
		return false;

	*ns = cycles < 0 ? -(s64)abs_ns : (s64)abs_ns;
	return true;
}

static bool tgpio_ns_delta_to_art(s64 ns, s64 *cycles)
{
	u64 abs_ns = tgpio_abs_s64(ns);
	u64 seconds;
	u64 rem;
	u64 abs_cycles;

	if (!art_frequency)
		return false;

	seconds = div64_u64_rem(abs_ns, NSEC_PER_SEC, &rem);
	if (seconds && art_frequency > div64_u64(U64_MAX, seconds))
		return false;

	abs_cycles = seconds * art_frequency +
		     div64_u64(rem * art_frequency, NSEC_PER_SEC);
	if (abs_cycles > S64_MAX)
		return false;

	*cycles = ns < 0 ? -(s64)abs_cycles : (s64)abs_cycles;
	return true;
}

static s64 tgpio_scaled_ppm_to_ppb(long scaled_ppm)
{
	if (scaled_ppm > div_s64(S64_MAX, 1000))
		return S64_MAX;
	if (scaled_ppm < div_s64(S64_MIN, 1000))
		return S64_MIN;

	return div_s64((s64)scaled_ppm * 1000, 1 << 16);
}

static s64 tgpio_apply_ppb_to_ns(s64 ns, s64 ppb)
{
	bool negative = (ns < 0) ^ (ppb < 0);
	u64 abs_ns = tgpio_abs_s64(ns);
	u64 abs_ppb = tgpio_abs_s64(ppb);
	u64 seconds;
	u64 rem;
	u64 correction;

	if (!ns || !ppb)
		return 0;

	seconds = div64_u64_rem(abs_ns, NSEC_PER_SEC, &rem);
	correction = seconds * abs_ppb +
		     div64_u64(rem * abs_ppb, NSEC_PER_SEC);

	if (correction > S64_MAX)
		correction = S64_MAX;

	return negative ? -(s64)correction : (s64)correction;
}

static bool tgpio_phc_base_to_adjusted_delta(s64 base_ns, s64 ppb,
					     s64 *adjusted_ns)
{
	s64 correction = tgpio_apply_ppb_to_ns(base_ns, ppb);

	return !tgpio_add_s64_overflow(base_ns, correction, adjusted_ns);
}

static bool tgpio_phc_adjusted_to_base_delta(s64 adjusted_ns, s64 ppb,
					     s64 *base_ns)
{
	bool negative = adjusted_ns < 0;
	u64 abs_adjusted = tgpio_abs_s64(adjusted_ns);
	s64 denominator_s64;
	u64 denominator;
	u64 quotient;
	u64 rem;
	u64 abs_base;

	denominator_s64 = (s64)NSEC_PER_SEC + ppb;
	if (denominator_s64 <= 0)
		return false;
	denominator = denominator_s64;

	quotient = div64_u64_rem(abs_adjusted, denominator, &rem);
	if (quotient > div64_u64(S64_MAX, NSEC_PER_SEC))
		return false;

	abs_base = quotient * NSEC_PER_SEC +
		   div64_u64(rem * NSEC_PER_SEC, denominator);
	if (abs_base > S64_MAX)
		return false;

	*base_ns = negative ? -(s64)abs_base : (s64)abs_base;
	return true;
}

static bool tgpio_get_current_art(u64 *art)
{
	return ktime_real_to_base_clock(ktime_get_real(), CSID_X86_ART, art);
}

static bool tgpio_phc_art_to_ns_locked(struct tgpio_state *state, u64 art,
				       s64 *phc_ns)
{
	struct tgpio_phc_clock *phc = &state->phc;
	s64 delta_cycles;
	s64 base_delta_ns;
	s64 adjusted_delta_ns;
	s64 ppb;

	if (art >= phc->anchor_art) {
		if (!tgpio_u64_to_s64(art - phc->anchor_art, &delta_cycles))
			return false;
	} else {
		if (!tgpio_u64_to_s64(phc->anchor_art - art, &delta_cycles))
			return false;
		delta_cycles = -delta_cycles;
	}

	if (!tgpio_art_delta_to_ns(delta_cycles, &base_delta_ns))
		return false;

	ppb = tgpio_scaled_ppm_to_ppb(phc->scaled_ppm);
	if (!tgpio_phc_base_to_adjusted_delta(base_delta_ns, ppb,
					      &adjusted_delta_ns))
		return false;

	return !tgpio_add_s64_overflow(phc->anchor_ns, adjusted_delta_ns,
				       phc_ns);
}

static bool tgpio_phc_art_to_ns(struct tgpio_state *state, u64 art,
				s64 *phc_ns)
{
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&state->phc.lock, flags);
	ret = tgpio_phc_art_to_ns_locked(state, art, phc_ns);
	spin_unlock_irqrestore(&state->phc.lock, flags);

	return ret;
}

static bool tgpio_phc_now_ns(struct tgpio_state *state, s64 *phc_ns)
{
	u64 art;

	if (!tgpio_get_current_art(&art))
		return false;

	return tgpio_phc_art_to_ns(state, art, phc_ns);
}

static bool tgpio_phc_ns_to_art(struct tgpio_state *state, s64 phc_ns,
				u64 *art)
{
	unsigned long flags;
	u64 anchor_art;
	s64 anchor_ns;
	long scaled_ppm;
	s64 phc_delta_ns;
	s64 base_delta_ns;
	s64 delta_cycles;
	s64 ppb;

	spin_lock_irqsave(&state->phc.lock, flags);
	anchor_art = state->phc.anchor_art;
	anchor_ns = state->phc.anchor_ns;
	scaled_ppm = state->phc.scaled_ppm;
	spin_unlock_irqrestore(&state->phc.lock, flags);

	if (tgpio_add_s64_overflow(phc_ns, -anchor_ns, &phc_delta_ns))
		return false;

	ppb = tgpio_scaled_ppm_to_ppb(scaled_ppm);
	if (!tgpio_phc_adjusted_to_base_delta(phc_delta_ns, ppb,
					      &base_delta_ns))
		return false;

	if (!tgpio_ns_delta_to_art(base_delta_ns, &delta_cycles))
		return false;

	if (delta_cycles >= 0) {
		*art = anchor_art + (u64)delta_cycles;
		if (*art < anchor_art)
			return false;
	} else {
		u64 abs_cycles = tgpio_abs_s64(delta_cycles);

		if (abs_cycles > anchor_art)
			return false;
		*art = anchor_art - abs_cycles;
	}

	return true;
}

static bool tgpio_phc_delta_to_real_ns(struct tgpio_state *state,
				       s64 phc_delta_ns, u64 *real_delta_ns)
{
	unsigned long flags;
	long scaled_ppm;
	s64 base_delta_ns;
	s64 ppb;

	if (phc_delta_ns <= 0) {
		*real_delta_ns = 0;
		return true;
	}

	spin_lock_irqsave(&state->phc.lock, flags);
	scaled_ppm = state->phc.scaled_ppm;
	spin_unlock_irqrestore(&state->phc.lock, flags);

	ppb = tgpio_scaled_ppm_to_ppb(scaled_ppm);
	if (!tgpio_phc_adjusted_to_base_delta(phc_delta_ns, ppb,
					      &base_delta_ns) ||
	    base_delta_ns < 0)
		return false;

	*real_delta_ns = base_delta_ns;
	return true;
}

static bool tgpio_clock_now_ns(struct tgpio_state *state, s64 *ns)
{
	if (clock_mode == TGPIO_CLOCK_PHC)
		return tgpio_phc_now_ns(state, ns);

	*ns = ktime_get_real_ns();
	return true;
}

static bool tgpio_clock_ns_to_art(struct tgpio_state *state, s64 ns, u64 *art)
{
	if (clock_mode == TGPIO_CLOCK_PHC)
		return tgpio_phc_ns_to_art(state, ns, art);

	return ktime_real_to_base_clock(ns_to_ktime(ns), CSID_X86_ART, art);
}

static bool tgpio_clock_delta_to_real_ns(struct tgpio_state *state,
					 s64 clock_delta_ns,
					 u64 *real_delta_ns)
{
	if (clock_mode == TGPIO_CLOCK_PHC)
		return tgpio_phc_delta_to_real_ns(state, clock_delta_ns,
						  real_delta_ns);

	if (clock_delta_ns <= 0)
		*real_delta_ns = 0;
	else
		*real_delta_ns = clock_delta_ns;
	return true;
}

static int tgpio_phc_init_clock(struct tgpio_state *state)
{
	ktime_t realtime;
	u64 art;

	realtime = ktime_get_real();
	if (!ktime_real_to_base_clock(realtime, CSID_X86_ART, &art))
		return -ENODEV;

	spin_lock_init(&state->phc.lock);
	state->phc.anchor_art = art;
	state->phc.anchor_ns = ktime_to_ns(realtime);
	state->phc.scaled_ppm = 0;
	return 0;
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

struct tgpio_crosststamp_ctx {
	u64 art_cycles;
};

static int tgpio_get_crosststamp(ktime_t *device_time,
				 struct system_counterval_t *sys_counterval,
				 void *ctx)
{
	struct tgpio_crosststamp_ctx *timestamp = ctx;

	*device_time = ns_to_ktime(0);
	sys_counterval->cycles = timestamp->art_cycles;
	sys_counterval->cs_id = CSID_X86_ART;
	sys_counterval->use_nsecs = false;

	return 0;
}

static bool tgpio_art_to_realtime_ns(struct tgpio_block *block, u64 art,
				     u64 *timestamp)
{
	struct tgpio_crosststamp_ctx ctx = {
		.art_cycles = art,
	};
	struct system_device_crosststamp xtstamp = { };
	struct system_time_snapshot *history = NULL;
	int ret;

	if (block->timestamp_history_valid)
		history = &block->timestamp_history;

	ret = get_device_system_crosststamp(tgpio_get_crosststamp, &ctx,
					    history, &xtstamp);
	if (ret)
		return false;

	*timestamp = ktime_to_ns(xtstamp.sys_realtime);
	return true;
}

static void
tgpio_snapshot_timestamp_history(struct system_time_snapshot *history,
				 bool *valid)
{
	if (!hardware_timestamps ||
	    clock_mode != TGPIO_CLOCK_REALTIME ||
	    timestamp_mode != TGPIO_TIMESTAMP_REALTIME) {
		*valid = false;
		return;
	}

	ktime_get_snapshot(history);
	*valid = true;
}

static void tgpio_save_timestamp_history(struct tgpio_block *block,
					 struct system_time_snapshot *history,
					 bool valid)
{
	if (!valid) {
		block->timestamp_history_valid = false;
		return;
	}

	block->timestamp_history = *history;
	block->timestamp_history_valid = true;
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

static void tgpio_emit_event(struct tgpio_state *state,
			     struct tgpio_block *block,
			     unsigned int index, u64 art_cycles)
{
	struct ptp_clock_event event = {
		.type = PTP_CLOCK_EXTTS,
		.index = index,
	};

	if (!state->clock)
		return;

	if (!hardware_timestamps) {
		s64 timestamp;

		if (tgpio_clock_now_ns(state, &timestamp) && timestamp >= 0)
			event.timestamp = timestamp;
		else
			event.timestamp = ktime_get_real_ns();
	} else if (clock_mode == TGPIO_CLOCK_PHC) {
		s64 timestamp;

		if (!tgpio_phc_art_to_ns(state, art_cycles, &timestamp) ||
		    timestamp < 0) {
			pr_warn_ratelimited("failed to convert ART capture to "
					    "adjusted PHC time; using poll time\n");
			if (!tgpio_clock_now_ns(state, &timestamp) ||
			    timestamp < 0)
				timestamp = ktime_get_real_ns();
		}
		event.timestamp = timestamp;
	} else if (timestamp_mode == TGPIO_TIMESTAMP_REALTIME) {
		if (!tgpio_art_to_realtime_ns(block, art_cycles,
					      &event.timestamp)) {
			pr_warn_ratelimited("failed to convert ART capture to "
					    "CLOCK_REALTIME; using poll time\n");
			event.timestamp = ktime_get_real_ns();
		}
	} else {
		event.timestamp = tgpio_art_to_ns(art_cycles);
	}

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
		struct system_time_snapshot history;
		bool history_valid;
		u64 count;
		u64 art;

		if (block->mode != TGPIO_MODE_INPUT || !block->enabled)
			continue;

		enabled = true;
		tgpio_snapshot_timestamp_history(&history, &history_valid);
		tgpio_read_capture(block, &count, &art);

		if (count != block->last_count) {
			unsigned int channel;

			block->last_count = count;
			channel = tgpio_channel_for_block(state, i,
							  PTP_PF_EXTTS);
			tgpio_emit_event(state, block, channel, art);
		}

		tgpio_save_timestamp_history(block, &history, history_valid);
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
		struct system_time_snapshot history;
		bool history_valid;

		ctrl &= ~TGPIOCTL_EP;
		ctrl |= tgpio_edge_bits(block, flags);
		ctrl |= TGPIOCTL_DIR;
		tgpio_writel(block, TGPIOCTL, ctrl);

		tgpio_read_capture(block, &count, &art);
		block->last_count = count;
		tgpio_snapshot_timestamp_history(&history, &history_valid);
		tgpio_save_timestamp_history(block, &history, history_valid);
		block->enabled = true;

		ctrl |= TGPIOCTL_EN;
		tgpio_writel(block, TGPIOCTL, ctrl);
		schedule_delayed_work(&state->poll_work,
				      msecs_to_jiffies(poll_ms ? poll_ms : 1));
	} else {
		block->enabled = false;
		block->timestamp_history_valid = false;
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

static int tgpio_ptp_time_to_s64_ns(const struct ptp_clock_time *time, s64 *ns)
{
	if (time->sec < 0 || time->nsec >= NSEC_PER_SEC)
		return -EINVAL;

	if (time->sec > div64_s64(S64_MAX, NSEC_PER_SEC))
		return -ERANGE;

	*ns = (s64)time->sec * NSEC_PER_SEC + time->nsec;
	return 0;
}

static bool tgpio_program_output_edge(struct tgpio_state *state,
				      struct tgpio_block *block,
				      ktime_t edge_time, u32 edge_bits)
{
	u64 art;
	u32 ctrl;

	if (!tgpio_clock_ns_to_art(state, ktime_to_ns(edge_time), &art))
		return false;

	if (art > TGPIO_ART_HW_DELAY_CYCLES)
		art -= TGPIO_ART_HW_DELAY_CYCLES;

	ctrl = tgpio_readl(block, TGPIOCTL);
	ctrl &= ~TGPIOCTL_EP;
	ctrl |= edge_bits;
	tgpio_writel(block, TGPIOCTL, ctrl);

	tgpio_write_compv(block, art);
	return true;
}

static int tgpio_prepare_output_timing(struct tgpio_state *state,
				       u64 half_period_ns,
				       s64 *first_edge_ns,
				       s64 *prime_edge_ns,
				       ktime_t *timer_start)
{
	s64 min_first_edge_ns;
	s64 now_ns;
	s64 timer_delay_clock_ns;
	s64 timer_start_ns;
	s64 lead_ns_s64;
	u64 lead_ns;
	u64 timer_delay_ns;
	ktime_t now;

	if (!tgpio_clock_now_ns(state, &now_ns))
		return -ENODEV;
	if (now_ns < 0)
		return -ERANGE;

	lead_ns = tgpio_output_lead_time_ns(half_period_ns);
	if (lead_ns > S64_MAX / 3)
		return -ERANGE;
	lead_ns_s64 = (s64)lead_ns;

	if (tgpio_add_s64_overflow(now_ns, 3 * lead_ns_s64,
				   &min_first_edge_ns))
		return -ERANGE;
	if (*first_edge_ns < min_first_edge_ns)
		*first_edge_ns = min_first_edge_ns;

	if (tgpio_add_s64_overflow(*first_edge_ns, -2 * lead_ns_s64,
				   prime_edge_ns) ||
	    tgpio_add_s64_overflow(*first_edge_ns, -lead_ns_s64,
				   &timer_start_ns) ||
	    tgpio_add_s64_overflow(timer_start_ns, -now_ns,
				   &timer_delay_clock_ns))
		return -ERANGE;

	if (!tgpio_clock_delta_to_real_ns(state, timer_delay_clock_ns,
					  &timer_delay_ns))
		return -ENODEV;

	now = ktime_get_real();
	*timer_start = ktime_add_ns(now, timer_delay_ns);
	if (ktime_before(*timer_start, now))
		*timer_start = now;

	return 0;
}

static void tgpio_disable_output_hw(struct tgpio_block *block)
{
	u32 ctrl;

	ctrl = tgpio_readl(block, TGPIOCTL);
	tgpio_write_compv(block, 0);
	ctrl &= ~TGPIOCTL_EN;
	tgpio_writel(block, TGPIOCTL, ctrl);
	block->output_enabled = false;
	block->output_phase = TGPIO_OUTPUT_TOGGLE;
}

static int tgpio_arm_output_locked(struct tgpio_state *state,
				   struct tgpio_block *block,
				   s64 first_edge_ns,
				   s64 prime_edge_ns,
				   u64 half_period_ns)
{
	u32 ctrl;

	ctrl = tgpio_readl(block, TGPIOCTL);
	ctrl &= ~TGPIOCTL_EN;
	tgpio_writel(block, TGPIOCTL, ctrl);
	tgpio_write_compv(block, 0);

	ctrl &= ~(TGPIOCTL_DIR | TGPIOCTL_EP | TGPIOCTL_PM);
	tgpio_writel(block, TGPIOCTL, ctrl);

	if (!tgpio_program_output_edge(state, block, ns_to_ktime(prime_edge_ns),
				       tgpio_output_edge_bits(false))) {
		tgpio_disable_output_hw(block);
		return -ENODEV;
	}

	block->output_half_period_ns = half_period_ns;
	block->output_next_edge = ns_to_ktime(first_edge_ns);
	block->output_phase = TGPIO_OUTPUT_FIRST_RISING;
	block->output_enabled = true;

	ctrl = tgpio_readl(block, TGPIOCTL);
	ctrl |= TGPIOCTL_EN;
	tgpio_writel(block, TGPIOCTL, ctrl);

	return 0;
}

static enum hrtimer_restart tgpio_output_timer(struct hrtimer *timer)
{
	struct tgpio_block *block =
		container_of(timer, struct tgpio_block, output_timer);
	struct tgpio_state *state = block->state;
	ktime_t now;
	ktime_t next_edge;
	u64 interval_ns;
	unsigned long flags;
	u32 edge_bits;

	spin_lock_irqsave(&block->output_lock, flags);
	if (!block->output_enabled) {
		spin_unlock_irqrestore(&block->output_lock, flags);
		return HRTIMER_NORESTART;
	}

	if (block->output_phase == TGPIO_OUTPUT_FIRST_RISING) {
		next_edge = block->output_next_edge;
		edge_bits = tgpio_output_edge_bits(true);
	} else {
		next_edge = ktime_add_ns(block->output_next_edge,
					 block->output_half_period_ns);
		edge_bits = TGPIOCTL_EP_TOGGLE;
	}

	if (!tgpio_program_output_edge(state, block, next_edge, edge_bits) ||
	    !tgpio_clock_delta_to_real_ns(state, block->output_half_period_ns,
					  &interval_ns) ||
	    !interval_ns) {
		tgpio_disable_output_hw(block);
		spin_unlock_irqrestore(&block->output_lock, flags);
		return HRTIMER_NORESTART;
	}

	block->output_next_edge = next_edge;
	block->output_phase = TGPIO_OUTPUT_TOGGLE;
	now = ktime_get_real();
	hrtimer_forward(timer, now, ns_to_ktime(interval_ns));
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
	s64 first_edge_ns;
	s64 prime_edge_ns;
	ktime_t timer_start;
	unsigned long irqflags;
	u64 period_ns;
	u64 half_period_ns;
	int ret;

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

	ret = tgpio_ptp_time_to_s64_ns(&perout->start, &first_edge_ns);
	if (ret)
		return ret;

	ret = tgpio_prepare_output_timing(state, half_period_ns,
					  &first_edge_ns, &prime_edge_ns,
					  &timer_start);
	if (ret)
		return ret;

	tgpio_disable_output(block);

	spin_lock_irqsave(&block->output_lock, irqflags);
	ret = tgpio_arm_output_locked(state, block, first_edge_ns,
				      prime_edge_ns, half_period_ns);
	if (ret) {
		spin_unlock_irqrestore(&block->output_lock, irqflags);
		return ret;
	}
	spin_unlock_irqrestore(&block->output_lock, irqflags);

	hrtimer_start(&block->output_timer, timer_start, HRTIMER_MODE_ABS);
	return 0;
}

static void tgpio_resync_outputs_after_phc_step(struct tgpio_state *state)
{
	unsigned int i;

	if (clock_mode != TGPIO_CLOCK_PHC)
		return;

	mutex_lock(&state->lock);
	for (i = 0; i < ARRAY_SIZE(state->blocks); i++) {
		struct tgpio_block *block = &state->blocks[i];
		unsigned long flags;
		u64 half_period_ns;
		s64 first_edge_ns = 0;
		s64 prime_edge_ns;
		ktime_t timer_start;
		int ret;
		u32 ctrl;

		if (block->mode != TGPIO_MODE_OUTPUT || !block->base)
			continue;

		hrtimer_cancel(&block->output_timer);

		spin_lock_irqsave(&block->output_lock, flags);
		if (!block->output_enabled) {
			spin_unlock_irqrestore(&block->output_lock, flags);
			continue;
		}
		half_period_ns = block->output_half_period_ns;
		ctrl = tgpio_readl(block, TGPIOCTL);
		tgpio_write_compv(block, 0);
		ctrl &= ~TGPIOCTL_EN;
		tgpio_writel(block, TGPIOCTL, ctrl);
		spin_unlock_irqrestore(&block->output_lock, flags);

		ret = tgpio_prepare_output_timing(state, half_period_ns,
						  &first_edge_ns,
						  &prime_edge_ns,
						  &timer_start);

		spin_lock_irqsave(&block->output_lock, flags);
		if (!block->output_enabled) {
			spin_unlock_irqrestore(&block->output_lock, flags);
			continue;
		}
		if (ret ||
		    tgpio_arm_output_locked(state, block, first_edge_ns,
					    prime_edge_ns, half_period_ns)) {
			tgpio_disable_output_hw(block);
			spin_unlock_irqrestore(&block->output_lock, flags);
			pr_warn_ratelimited("failed to resync output block %u after PHC step\n",
					    i);
			continue;
		}
		spin_unlock_irqrestore(&block->output_lock, flags);

		hrtimer_start(&block->output_timer, timer_start,
			      HRTIMER_MODE_ABS);
	}
	mutex_unlock(&state->lock);
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
	struct tgpio_state *state =
		container_of(ptp, struct tgpio_state, info);
	s64 now_ns;

	if (clock_mode == TGPIO_CLOCK_PHC) {
		if (!tgpio_phc_now_ns(state, &now_ns))
			return -ENODEV;
		if (now_ns < 0)
			return -ERANGE;

		*ts = ns_to_timespec64(now_ns);
		return 0;
	}

	ktime_get_real_ts64(ts);
	return 0;
}

static int tgpio_ptp_settime64(struct ptp_clock_info *ptp,
			       const struct timespec64 *ts)
{
	struct tgpio_state *state =
		container_of(ptp, struct tgpio_state, info);
	unsigned long flags;
	s64 ns;
	u64 art;

	if (clock_mode != TGPIO_CLOCK_PHC)
		return -EOPNOTSUPP;

	if (ts->tv_sec < 0 || ts->tv_nsec < 0 ||
	    ts->tv_nsec >= NSEC_PER_SEC)
		return -EINVAL;
	if (ts->tv_sec > div64_s64(S64_MAX, NSEC_PER_SEC))
		return -ERANGE;

	ns = timespec64_to_ns(ts);
	if (ns < 0)
		return -ERANGE;

	if (!tgpio_get_current_art(&art))
		return -ENODEV;

	spin_lock_irqsave(&state->phc.lock, flags);
	state->phc.anchor_art = art;
	state->phc.anchor_ns = ns;
	spin_unlock_irqrestore(&state->phc.lock, flags);

	tgpio_resync_outputs_after_phc_step(state);
	return 0;
}

static int tgpio_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct tgpio_state *state =
		container_of(ptp, struct tgpio_state, info);
	unsigned long flags;
	s64 now_ns;
	s64 adjusted_ns;
	u64 art;

	if (clock_mode != TGPIO_CLOCK_PHC)
		return -EOPNOTSUPP;

	if (!tgpio_get_current_art(&art))
		return -ENODEV;

	spin_lock_irqsave(&state->phc.lock, flags);
	if (!tgpio_phc_art_to_ns_locked(state, art, &now_ns) ||
	    tgpio_add_s64_overflow(now_ns, delta, &adjusted_ns)) {
		spin_unlock_irqrestore(&state->phc.lock, flags);
		return -ERANGE;
	}
	if (adjusted_ns < 0) {
		spin_unlock_irqrestore(&state->phc.lock, flags);
		return -ERANGE;
	}

	state->phc.anchor_art = art;
	state->phc.anchor_ns = adjusted_ns;
	spin_unlock_irqrestore(&state->phc.lock, flags);

	tgpio_resync_outputs_after_phc_step(state);
	return 0;
}

static int tgpio_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct tgpio_state *state =
		container_of(ptp, struct tgpio_state, info);
	unsigned long flags;
	s64 now_ns;
	s64 ppb;
	u64 art;

	if (clock_mode != TGPIO_CLOCK_PHC)
		return -EOPNOTSUPP;

	ppb = tgpio_scaled_ppm_to_ppb(scaled_ppm);
	if (ppb > TGPIO_PHC_MAX_ADJ_PPB ||
	    ppb < -TGPIO_PHC_MAX_ADJ_PPB)
		return -ERANGE;

	if (!tgpio_get_current_art(&art))
		return -ENODEV;

	spin_lock_irqsave(&state->phc.lock, flags);
	if (!tgpio_phc_art_to_ns_locked(state, art, &now_ns)) {
		spin_unlock_irqrestore(&state->phc.lock, flags);
		return -ERANGE;
	}

	state->phc.anchor_art = art;
	state->phc.anchor_ns = now_ns;
	state->phc.scaled_ppm = scaled_ppm;
	spin_unlock_irqrestore(&state->phc.lock, flags);

	return 0;
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

		block->state = state;
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

	if (timestamp_mode != TGPIO_TIMESTAMP_ART)
		return false;

	for (i = 0; i < ARRAY_SIZE(state->blocks); i++) {
		if (state->blocks[i].mode == TGPIO_MODE_INPUT)
			return true;
	}

	return false;
}

static bool tgpio_needs_art_base_clock(struct tgpio_state *state)
{
	unsigned int i;

	if (!hardware_timestamps)
		return false;

	if (timestamp_mode != TGPIO_TIMESTAMP_REALTIME)
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
	state->info.max_adj = clock_mode == TGPIO_CLOCK_PHC ?
			      TGPIO_PHC_MAX_ADJ_PPB : 0;
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

	ret = tgpio_parse_clock_mode(clock_mode_param, &clock_mode);
	if (ret) {
		pr_err("invalid clock_mode=%s; use realtime or phc\n",
		       clock_mode_param);
		goto err_cleanup;
	}

	ret = tgpio_parse_timestamp_mode(timestamp_mode_param, &timestamp_mode);
	if (ret) {
		pr_err("invalid timestamp_mode=%s; use realtime or art\n",
		       timestamp_mode_param);
		goto err_cleanup;
	}

	ret = tgpio_parse_output_polarity(output_polarity_param,
					  &output_polarity);
	if (ret) {
		pr_err("invalid output_polarity=%s; use normal or inverted\n",
		       output_polarity_param);
		goto err_cleanup;
	}

	ret = tgpio_configure_blocks(tgpio);
	if (ret)
		goto err_cleanup;

	if (clock_mode == TGPIO_CLOCK_PHC) {
		if (!timekeeping_clocksource_has_base(CSID_X86_ART)) {
			pr_err("clock_mode=phc requires a timekeeper clocksource "
			       "based on ART\n");
			ret = -ENODEV;
			goto err_cleanup;
		}

		ret = tgpio_resolve_art_frequency();
		if (ret)
			goto err_cleanup;

		ret = tgpio_phc_init_clock(tgpio);
		if (ret)
			goto err_cleanup;

		pr_info("PTP clock uses adjustable ART-backed PHC mode\n");
	} else if (tgpio_needs_art_frequency(tgpio)) {
		ret = tgpio_resolve_art_frequency();
		if (ret)
			goto err_cleanup;
	} else if (tgpio_needs_art_base_clock(tgpio)) {
		if (!timekeeping_clocksource_has_base(CSID_X86_ART)) {
			pr_err("timestamp_mode=realtime requires a timekeeper "
			       "clocksource based on ART; use timestamp_mode=art "
			       "or hardware_timestamps=0\n");
			ret = -ENODEV;
			goto err_cleanup;
		}
		tgpio_probe_art_parameters_for_status();
		pr_info("hardware input timestamps use CLOCK_REALTIME via ART base clock\n");
	} else if (!art_frequency) {
		pr_info("ART frequency not needed for this configuration\n");
	}

	ret = tgpio_register_ptp_clock(tgpio);
	if (ret)
		goto err_cleanup;

	pr_info("loaded with mode0=%s mode1=%s clock_mode=%s timestamp_mode=%s output_polarity=%s\n",
		tgpio_mode_name(tgpio->blocks[0].mode),
		tgpio_mode_name(tgpio->blocks[1].mode),
		tgpio_clock_mode_name(clock_mode),
		tgpio_timestamp_mode_name(timestamp_mode),
		tgpio_output_polarity_name(output_polarity));
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

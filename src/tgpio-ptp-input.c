// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/*
 * Intel Time-Aware GPIO PTP input/output add-on driver.
 * Copyright (c) 2026 Ahmad Byagowi
 *
 * This out-of-tree driver is for systems where TGPIO hardware is present but
 * firmware does not expose the ACPI devices needed for enumeration. Each known
 * static MMIO block can be assigned to PTP external timestamp input, PTP
 * periodic output, or left off.
 *
 * Organised in layers: a pure value layer (clock math, no MMIO), a hardware
 * layer (the only MMIO), glue (locking and PTP callbacks), and observability
 * (debugfs/sysfs/tracepoints).
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/clocksource_ids.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kobject.h>
#include <linux/kstrtox.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/seq_file.h>
#include <linux/seqlock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/timekeeping.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#ifdef CONFIG_X86
#include <asm/cpuid/api.h>
#endif

/*
 * v7.2 renamed the cross-timestamp API: ktime_get_snapshot() ->
 * ktime_get_snapshot_id(clock_id, ...), and system_device_crosststamp's
 * sys_realtime -> sys_systime. We always want CLOCK_REALTIME. The 7.2 cutoff
 * is empirical: the 7.0.0 target still has the old names.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
static inline void
tgpio_get_realtime_snapshot(struct system_time_snapshot *snapshot)
{
	ktime_get_snapshot_id(CLOCK_REALTIME, snapshot);
}
static inline bool
tgpio_snapshot_art_cycles(const struct system_time_snapshot *snapshot,
			  u64 *cycles)
{
	if (!snapshot->valid)
		return false;
	if (snapshot->hw_csid == CSID_X86_ART) {
		*cycles = snapshot->hw_cycles;
		return true;
	}
	if (snapshot->cs_id == CSID_X86_ART) {
		*cycles = snapshot->cycles;
		return true;
	}
	return false;
}
#define tgpio_xtstamp_realtime(xtstamp) ((xtstamp).sys_systime)
#define tgpio_snapshot_realtime(snapshot) ((snapshot).systime)
#else
static inline void
tgpio_get_realtime_snapshot(struct system_time_snapshot *snapshot)
{
	ktime_get_snapshot(snapshot);
}
static inline bool
tgpio_snapshot_art_cycles(const struct system_time_snapshot *snapshot,
			  u64 *cycles)
{
	if (snapshot->cs_id != CSID_X86_ART)
		return false;

	*cycles = snapshot->cycles;
	return true;
}
#define tgpio_xtstamp_realtime(xtstamp) ((xtstamp).sys_realtime)
#define tgpio_snapshot_realtime(snapshot) ((snapshot).real)
#endif

#define CREATE_TRACE_POINTS
#include "tgpio-trace.h"

#define TGPIO_MAX_BLOCKS 2

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

#define TGPIOCTL_EN	    BIT(0)
#define TGPIOCTL_DIR	    BIT(1)
#define TGPIOCTL_EP	    GENMASK(3, 2)
#define TGPIOCTL_EP_RISING  (0u << 2)
#define TGPIOCTL_EP_FALLING (1u << 2)
#define TGPIOCTL_EP_TOGGLE  (2u << 2)
#define TGPIOCTL_PM	    BIT(4)

#define TGPIO_ART_HW_DELAY_CYCLES 2
#define TGPIO_OUTPUT_SAFE_TIME_NS (20 * NSEC_PER_MSEC)
#define TGPIO_OUTPUT_PHASE_NUDGE_NS 25
#define TGPIO_CPUID_ART_LEAF	  0x15
#define TGPIO_PHC_MAX_ADJ_PPB	  100000000L
#define TGPIO_MIN_POLL_MS	  1u
#define TGPIO_MIN_MMIO_SIZE	  0x38u
#define TGPIO_INPUT_DESIRED_ON	  BIT(1) /* edge bits live in TGPIOCTL_EP */

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
	TGPIO_OUTPUT_ARM_PERIODIC,
	TGPIO_OUTPUT_HARDWARE_PERIODIC,
	TGPIO_OUTPUT_FIRST_RISING,
	TGPIO_OUTPUT_TOGGLE,
};

enum tgpio_output_polarity {
	TGPIO_OUTPUT_NORMAL,
	TGPIO_OUTPUT_INVERTED,
};

enum tgpio_capture_state {
	TGPIO_CAPTURE_OFF,
	TGPIO_CAPTURE_ON,
};

enum tgpio_output_run {
	TGPIO_OUTPUT_STOPPED,
	TGPIO_OUTPUT_RUNNING,
};

enum tgpio_snapshot_state {
	TGPIO_SNAPSHOT_NONE,
	TGPIO_SNAPSHOT_PRESENT,
};

/* Value is the errno itself: <0 error, >=0 usable (FALLBACK = degraded). */
enum tgpio_status {
	TGPIO_OK = 0,
	TGPIO_FALLBACK = 1,
	TGPIO_E_INVAL = -EINVAL,
	TGPIO_E_RANGE = -ERANGE,
	TGPIO_E_NODEV = -ENODEV,
};

enum tgpio_logical_edge {
	TGPIO_EDGE_FALL,
	TGPIO_EDGE_RISE,
};

enum tgpio_support {
	TGPIO_UNSUPPORTED,
	TGPIO_SUPPORTED,
};

enum tgpio_requirement {
	TGPIO_NOT_REQUIRED,
	TGPIO_REQUIRED,
};

enum tgpio_output_mode {
	TGPIO_OUTPUT_SOFTWARE,
	TGPIO_OUTPUT_HARDWARE,
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
static bool hardware_periodic_output = true;
static bool activity_log;
static bool input0_enable;
static bool input1_enable;
static unsigned int input0_channel;
static unsigned int input1_channel = 1;
static unsigned int output0_channel;
static unsigned int output1_channel = 1;
static unsigned long output0_period_ns;
static unsigned long output1_period_ns;
static unsigned long output0_duty_ns;
static unsigned long output1_duty_ns;
static unsigned long output_start_delay_ns;
static long output_phase_offset_ns;

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
MODULE_PARM_DESC(edge0,
		 "Default input edge for block 0: rising, falling, or both");

module_param_string(edge1, edge1_param, sizeof(edge1_param), 0444);
MODULE_PARM_DESC(edge1,
		 "Default input edge for block 1: rising, falling, or both");

module_param_string(clock_mode, clock_mode_param, sizeof(clock_mode_param),
		    0444);
MODULE_PARM_DESC(clock_mode, "PTP clock mode: realtime or phc");

module_param_string(timestamp_mode, timestamp_mode_param,
		    sizeof(timestamp_mode_param), 0444);
MODULE_PARM_DESC(timestamp_mode, "Hardware timestamp mode: realtime or art");

module_param_string(output_polarity, output_polarity_param,
		    sizeof(output_polarity_param), 0444);
MODULE_PARM_DESC(output_polarity,
		 "Periodic output polarity: normal or inverted");

module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "Polling interval for captured input events");

module_param(art_frequency, ulong, 0444);
MODULE_PARM_DESC(
	art_frequency,
	"ART frequency in Hz used to convert capture cycles to nanoseconds; 0 auto-detects from CPUID leaf 0x15");

module_param(tsc_art_numerator, uint, 0444);
MODULE_PARM_DESC(tsc_art_numerator,
		 "Detected CPUID leaf 0x15 TSC/ART numerator; 0 unknown");

module_param(tsc_art_denominator, uint, 0444);
MODULE_PARM_DESC(tsc_art_denominator,
		 "Detected CPUID leaf 0x15 TSC/ART denominator; 0 unknown");

module_param(hardware_timestamps, bool, 0644);
MODULE_PARM_DESC(hardware_timestamps,
		 "Use hardware capture time instead of poll time");

module_param(hardware_periodic_output, bool, 0444);
MODULE_PARM_DESC(hardware_periodic_output,
		 "Use TGPIO hardware periodic mode for PTP periodic output");

module_param(activity_log, bool, 0644);
MODULE_PARM_DESC(activity_log,
		 "Log input captures and output activity to the kernel journal");

module_param(input0_enable, bool, 0444);
MODULE_PARM_DESC(input0_enable,
		 "Enable external timestamp capture on input block 0 at load");

module_param(input1_enable, bool, 0444);
MODULE_PARM_DESC(input1_enable,
		 "Enable external timestamp capture on input block 1 at load");

module_param(input0_channel, uint, 0444);
MODULE_PARM_DESC(input0_channel,
		 "PTP external timestamp channel for persisted input block 0");

module_param(input1_channel, uint, 0444);
MODULE_PARM_DESC(input1_channel,
		 "PTP external timestamp channel for persisted input block 1");

module_param(output0_channel, uint, 0444);
MODULE_PARM_DESC(output0_channel,
		 "PTP periodic-output channel for persisted output block 0");

module_param(output1_channel, uint, 0444);
MODULE_PARM_DESC(output1_channel,
		 "PTP periodic-output channel for persisted output block 1");

module_param(output0_period_ns, ulong, 0444);
MODULE_PARM_DESC(output0_period_ns,
		 "Start block 0 periodic output at load with this period in ns; 0 disables");

module_param(output0_duty_ns, ulong, 0444);
MODULE_PARM_DESC(output0_duty_ns,
		 "Block 0 persisted output on/high time in ns; 0 means 50 percent duty");

module_param(output1_period_ns, ulong, 0444);
MODULE_PARM_DESC(output1_period_ns,
		 "Start block 1 periodic output at load with this period in ns; 0 disables");

module_param(output1_duty_ns, ulong, 0444);
MODULE_PARM_DESC(output1_duty_ns,
		 "Block 1 persisted output on/high time in ns; 0 means 50 percent duty");

module_param(output_start_delay_ns, ulong, 0444);
MODULE_PARM_DESC(output_start_delay_ns,
		 "Optional delay from current PTP time to first persisted output edge");

module_param(output_phase_offset_ns, long, 0644);
MODULE_PARM_DESC(output_phase_offset_ns,
		 "Calibration offset applied to programmed output edges in ns; positive moves the physical edge later. Writable at runtime; a running output converges within one or two frequency updates");

/* PHC time = anchor_ns + (ART cycles since anchor_art) scaled by scaled_ppm. */
struct tgpio_phc_params {
	u64 anchor_art;
	s64 anchor_ns;
	u64 base_art_hz;
	long scaled_ppm;
};

struct tgpio_s64_result {
	enum tgpio_status status;
	s64 val;
};
struct tgpio_u64_result {
	enum tgpio_status status;
	u64 val;
};

/* Unit-tagged results so a conversion's domain is clear from its type. */
struct tgpio_ns_result {
	enum tgpio_status status;
	u64 ns;
};

struct tgpio_capture {
	u64 event_count;
	u64 art_cycles;
};

struct tgpio_stats {
	atomic64_t events;
	atomic64_t fallbacks;
};

struct tgpio_output_timing {
	enum tgpio_status status;
	s64 first_edge_ns;
	s64 prime_edge_ns;
	ktime_t timer_start;
};

struct tgpio_output_quantization {
	enum tgpio_status status;
	u64 clock_half_period_ns;
	u64 art_half_period_cycles;
	u64 actual_half_period_ns;
	u64 actual_period_ns;
	s64 half_period_error_ns;
	s64 period_error_ns;
	s64 period_split_error_ns;
};

struct tgpio_crosststamp_capture {
	enum tgpio_snapshot_state state;
	struct system_time_snapshot snapshot;
};

/*
 * Output config published by enable/phc-step under output_seqlock, reconciled
 * by tgpio_output_work. Every publisher bumps gen; the work applies the latest.
 */
struct tgpio_output_desired {
	unsigned int flip_gen; /* polarity-calibration requests */
	unsigned int gen;
	unsigned int freq_gen;
	enum tgpio_output_run run;
	enum tgpio_output_mode mode;
	u64 period_ns;
	u64 high_time_ns;
	u64 low_time_ns;
	s64 first_edge_ns;
};

struct tgpio_output_times {
	enum tgpio_status status;
	u64 period_ns;
	u64 high_time_ns;
	u64 low_time_ns;
};

/*
 * One time-aware GPIO line: a single MMIO register block that is configured
 * either as a capture input or a periodic output.
 */
struct tgpio_mmio_block {
	struct tgpio_device *owner;
	unsigned int index;
	unsigned long mmio_phys;
	enum tgpio_mode mode;
	void __iomem *regs;
	struct resource *mem_region;
	u32 input_edge_bits;	/* default edge from module param */
	atomic_t input_desired; /* published by enable: ON bit | edge bits */
	int input_applied; /* reconciler-private: last programmed desired */
	u64 last_event_count;
	struct system_time_snapshot crosststamp_snapshot;
	enum tgpio_snapshot_state crosststamp_snapshot_state;
	struct hrtimer output_timer; /* precise wake -> schedules output_work */
	struct work_struct output_work; /* sole writer of output hw + the timer */
	seqlock_t output_seqlock;	/* guards output_desired writers */
	struct tgpio_output_desired output_desired;
	unsigned int output_applied_gen; /* output_work-private from here down */
	unsigned int output_applied_freq_gen;
	unsigned int output_applied_flip_gen;
	enum tgpio_output_phase output_phase;
	u64 output_hw_first_art; /* armed COMPV; edges at first + k * piv */
	u64 output_hw_piv;	 /* armed PIV; 0 = not in hardware periodic */
	u64 output_hw_ckpt_compv; /* COMPV at last flop checkpoint */
	bool output_hw_flop_high; /* tracked output level flop */
	u64 output_high_time_ns;
	u64 output_low_time_ns;
	enum tgpio_logical_edge output_next_edge_type;
	ktime_t output_next_edge;
	ktime_t output_wake_at; /* when the armed timer should fire */
};

struct tgpio_device {
	struct ptp_clock_info ptp_info;
	struct ptp_clock *ptp_clock;
	struct ptp_pin_desc pin_config[TGPIO_MAX_BLOCKS];
	seqlock_t phc_seqlock;
	struct tgpio_phc_params phc;
	struct tgpio_stats stats;
	struct delayed_work poll_work;
	unsigned int n_ptp_pins;
	struct tgpio_mmio_block mmio_blocks[TGPIO_MAX_BLOCKS];
};

static struct tgpio_device *tgpio;

static struct tgpio_u64_result tgpio_realtime_delta_to_art_cycles(u64 delta_ns);
static struct tgpio_ns_result tgpio_art_cycles_to_realtime_delta_ns(u64 cycles);

static inline struct tgpio_s64_result tgpio_ok_s64(s64 v)
{
	return (struct tgpio_s64_result){ TGPIO_OK, v };
}
static inline struct tgpio_s64_result tgpio_err_s64(enum tgpio_status e)
{
	return (struct tgpio_s64_result){ e, 0 };
}
static inline struct tgpio_u64_result tgpio_ok_u64(u64 v)
{
	return (struct tgpio_u64_result){ TGPIO_OK, v };
}
static inline struct tgpio_u64_result tgpio_err_u64(enum tgpio_status e)
{
	return (struct tgpio_u64_result){ e, 0 };
}

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

static const char *tgpio_edge_bits_name(u32 edge_bits)
{
	switch (edge_bits & TGPIOCTL_EP) {
	case TGPIOCTL_EP_RISING:
		return "rising";
	case TGPIOCTL_EP_FALLING:
		return "falling";
	case TGPIOCTL_EP_TOGGLE:
		return "both";
	default:
		return "unknown";
	}
}

static const char *tgpio_output_mode_name(enum tgpio_output_mode mode)
{
	switch (mode) {
	case TGPIO_OUTPUT_SOFTWARE:
		return "software";
	case TGPIO_OUTPUT_HARDWARE:
		return "hardware";
	default:
		return "unknown";
	}
}

static bool tgpio_persistent_input_enabled(unsigned int block_index)
{
	return block_index == 0 ? input0_enable : input1_enable;
}

static unsigned int tgpio_persistent_input_channel(unsigned int block_index)
{
	return block_index == 0 ? input0_channel : input1_channel;
}

static u64 tgpio_persistent_output_period_ns(unsigned int block_index)
{
	return block_index == 0 ? output0_period_ns : output1_period_ns;
}

static u64 tgpio_persistent_output_duty_ns(unsigned int block_index)
{
	return block_index == 0 ? output0_duty_ns : output1_duty_ns;
}

static unsigned int tgpio_persistent_output_channel(unsigned int block_index)
{
	return block_index == 0 ? output0_channel : output1_channel;
}

struct tgpio_mode_result {
	enum tgpio_status status;
	enum tgpio_mode mode;
};

static struct tgpio_mode_result tgpio_parse_mode(const char *value)
{
	if (sysfs_streq(value, "input") || sysfs_streq(value, "in"))
		return (struct tgpio_mode_result){ TGPIO_OK, TGPIO_MODE_INPUT };
	if (sysfs_streq(value, "output") || sysfs_streq(value, "out") ||
	    sysfs_streq(value, "pps"))
		return (struct tgpio_mode_result){ TGPIO_OK,
						   TGPIO_MODE_OUTPUT };
	if (sysfs_streq(value, "off") || sysfs_streq(value, "none") ||
	    sysfs_streq(value, "disable") || sysfs_streq(value, "disabled"))
		return (struct tgpio_mode_result){ TGPIO_OK, TGPIO_MODE_OFF };
	return (struct tgpio_mode_result){ .status = TGPIO_E_INVAL };
}

struct tgpio_edge_result {
	enum tgpio_status status;
	u32 edge_bits;
};

static struct tgpio_edge_result tgpio_parse_edge(const char *value)
{
	if (sysfs_streq(value, "rising") || sysfs_streq(value, "rise"))
		return (struct tgpio_edge_result){ TGPIO_OK,
						   TGPIOCTL_EP_RISING };
	if (sysfs_streq(value, "falling") || sysfs_streq(value, "fall"))
		return (struct tgpio_edge_result){ TGPIO_OK,
						   TGPIOCTL_EP_FALLING };
	if (sysfs_streq(value, "both") || sysfs_streq(value, "toggle") ||
	    sysfs_streq(value, "all"))
		return (struct tgpio_edge_result){ TGPIO_OK,
						   TGPIOCTL_EP_TOGGLE };
	return (struct tgpio_edge_result){ .status = TGPIO_E_INVAL };
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

struct tgpio_clock_mode_result {
	enum tgpio_status status;
	enum tgpio_clock_mode mode;
};

static struct tgpio_clock_mode_result tgpio_parse_clock_mode(const char *value)
{
	if (sysfs_streq(value, "realtime") ||
	    sysfs_streq(value, "clock_realtime") || sysfs_streq(value, "real"))
		return (struct tgpio_clock_mode_result){ TGPIO_OK,
							 TGPIO_CLOCK_REALTIME };
	if (sysfs_streq(value, "phc") || sysfs_streq(value, "adjustable") ||
	    sysfs_streq(value, "adjusted"))
		return (struct tgpio_clock_mode_result){ TGPIO_OK,
							 TGPIO_CLOCK_PHC };
	return (struct tgpio_clock_mode_result){ .status = TGPIO_E_INVAL };
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

struct tgpio_timestamp_mode_result {
	enum tgpio_status status;
	enum tgpio_timestamp_mode mode;
};

static struct tgpio_timestamp_mode_result
tgpio_parse_timestamp_mode(const char *value)
{
	if (sysfs_streq(value, "realtime") ||
	    sysfs_streq(value, "clock_realtime") || sysfs_streq(value, "real"))
		return (struct tgpio_timestamp_mode_result){
			TGPIO_OK, TGPIO_TIMESTAMP_REALTIME
		};
	if (sysfs_streq(value, "art") || sysfs_streq(value, "raw"))
		return (struct tgpio_timestamp_mode_result){
			TGPIO_OK, TGPIO_TIMESTAMP_ART
		};
	return (struct tgpio_timestamp_mode_result){ .status = TGPIO_E_INVAL };
}

static const char *
tgpio_output_polarity_name(enum tgpio_output_polarity polarity)
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

struct tgpio_output_polarity_result {
	enum tgpio_status status;
	enum tgpio_output_polarity polarity;
};

static struct tgpio_output_polarity_result
tgpio_parse_output_polarity(const char *value)
{
	if (sysfs_streq(value, "normal") || sysfs_streq(value, "active_high"))
		return (struct tgpio_output_polarity_result){
			TGPIO_OK, TGPIO_OUTPUT_NORMAL
		};
	if (sysfs_streq(value, "inverted") || sysfs_streq(value, "invert") ||
	    sysfs_streq(value, "active_low"))
		return (struct tgpio_output_polarity_result){
			TGPIO_OK, TGPIO_OUTPUT_INVERTED
		};
	return (struct tgpio_output_polarity_result){ .status = TGPIO_E_INVAL };
}

static u32 tgpio_output_edge_bits(enum tgpio_logical_edge edge)
{
	/*
	 * On the confirmed platform the compare EP bit is inverted relative to
	 * the logical edge; output_polarity=inverted cancels that inversion.
	 */
	enum tgpio_logical_edge inverted =
		edge == TGPIO_EDGE_RISE ? TGPIO_EDGE_FALL : TGPIO_EDGE_RISE;
	enum tgpio_logical_edge program =
		output_polarity == TGPIO_OUTPUT_INVERTED ? edge : inverted;

	if (program == TGPIO_EDGE_RISE)
		return TGPIOCTL_EP_RISING;
	return TGPIOCTL_EP_FALLING;
}

static u32 tgpio_ctl_with(u32 ctrl, u32 bits)
{
	return ctrl | bits;
}

static u32 tgpio_ctl_without(u32 ctrl, u32 bits)
{
	return ctrl & ~bits;
}

static u32 tgpio_ctl_with_edge(u32 ctrl, u32 edge_bits)
{
	return (ctrl & ~TGPIOCTL_EP) | edge_bits;
}

static u32 tgpio_input_edge_bits(struct tgpio_mmio_block *mmio_block,
				 unsigned int flags)
{
	unsigned int rising = flags & PTP_RISING_EDGE;
	unsigned int falling = flags & PTP_FALLING_EDGE;

	if (!rising && !falling)
		return mmio_block->input_edge_bits;

	if (rising && !falling)
		return TGPIOCTL_EP_RISING;
	if (!rising && falling)
		return TGPIOCTL_EP_FALLING;

	return TGPIOCTL_EP_TOGGLE;
}

static int tgpio_check_addr(unsigned long addr)
{
	if (!addr || mmio_size < TGPIO_MIN_MMIO_SIZE)
		return -EINVAL;

	if (addr + mmio_size - 1 < addr)
		return -EOVERFLOW;

	return 0;
}

static u64 tgpio_output_lead_time_ns(u64 interval_ns)
{
	u64 lead = min_t(u64, TGPIO_OUTPUT_SAFE_TIME_NS,
			 div64_u64(interval_ns, 4));

	if (!lead)
		lead = 1;

	return lead;
}

static enum tgpio_logical_edge
tgpio_output_opposite_edge(enum tgpio_logical_edge edge)
{
	return edge == TGPIO_EDGE_RISE ? TGPIO_EDGE_FALL : TGPIO_EDGE_RISE;
}

static u64
tgpio_output_interval_after_edge(struct tgpio_mmio_block *mmio_block,
				 enum tgpio_logical_edge edge)
{
	return edge == TGPIO_EDGE_RISE ? mmio_block->output_high_time_ns :
					 mmio_block->output_low_time_ns;
}

static struct tgpio_output_times
tgpio_output_times_err(enum tgpio_status status)
{
	return (struct tgpio_output_times){ .status = status };
}

static struct tgpio_output_times
tgpio_output_times_get(u64 period_ns, u64 duty_ns, bool duty_valid)
{
	u64 high_time_ns;
	u64 low_time_ns;

	if (!period_ns)
		return tgpio_output_times_err(TGPIO_E_INVAL);
	if (period_ns > S64_MAX)
		return tgpio_output_times_err(TGPIO_E_RANGE);

	high_time_ns = duty_valid ? duty_ns : div64_u64(period_ns, 2);
	if (!high_time_ns || high_time_ns >= period_ns)
		return tgpio_output_times_err(TGPIO_E_INVAL);

	low_time_ns = period_ns - high_time_ns;
	if (!low_time_ns || low_time_ns > S64_MAX)
		return tgpio_output_times_err(TGPIO_E_RANGE);

	return (struct tgpio_output_times){
		.status = TGPIO_OK,
		.period_ns = period_ns,
		.high_time_ns = high_time_ns,
		.low_time_ns = low_time_ns,
	};
}

static u64 tgpio_output_min_interval_ns(u64 high_time_ns, u64 low_time_ns)
{
	return min(high_time_ns, low_time_ns);
}

static bool tgpio_output_can_use_hardware(const struct tgpio_output_times *times)
{
	return times->high_time_ns == times->low_time_ns;
}

static unsigned int tgpio_supported_perout_flags(void)
{
	unsigned int flags = 0;

#ifdef PTP_PEROUT_DUTY_CYCLE
	flags |= PTP_PEROUT_DUTY_CYCLE;
#endif

	return flags;
}

/*
 * count * dst_hz / src_hz, split at the whole-second boundary so the products
 * stay in range. Internal core behind the typed converters below.
 */
static struct tgpio_u64_result
tgpio_rescale_common(u64 count, u64 src_hz, u64 dst_hz, bool round_nearest)
{
	u64 seconds;
	u64 sub_second; /* leftover src-domain units, < src_hz */
	u64 scaled_sub;
	u64 remainder = 0;
	u64 result;

	/*
	 * Guard the rates we were handed, not the global: callers read
	 * art_frequency once, so each conversion uses one consistent value.
	 */
	if (!src_hz || !dst_hz)
		return tgpio_err_u64(TGPIO_E_NODEV);

	seconds = div64_u64_rem(count, src_hz, &sub_second);
	if (seconds > div64_u64(S64_MAX, dst_hz))
		return tgpio_err_u64(TGPIO_E_RANGE);

	result = seconds * dst_hz;
	scaled_sub = div64_u64_rem(sub_second * dst_hz, src_hz, &remainder);
	if (round_nearest && remainder >= src_hz - remainder)
		scaled_sub++;

	if (scaled_sub > S64_MAX - result)
		return tgpio_err_u64(TGPIO_E_RANGE);

	return tgpio_ok_u64(result + scaled_sub);
}

static struct tgpio_u64_result tgpio_rescale(u64 count, u64 src_hz, u64 dst_hz)
{
	return tgpio_rescale_common(count, src_hz, dst_hz, false);
}

static struct tgpio_u64_result
tgpio_rescale_nearest(u64 count, u64 src_hz, u64 dst_hz)
{
	return tgpio_rescale_common(count, src_hz, dst_hz, true);
}

static struct tgpio_ns_result tgpio_cycles_to_ns(u64 cycles)
{
	struct tgpio_u64_result r =
		tgpio_rescale(cycles, READ_ONCE(art_frequency), NSEC_PER_SEC);

	return (struct tgpio_ns_result){ .status = r.status, .ns = r.val };
}

static u64 tgpio_art_to_ns(u64 art)
{
	struct tgpio_ns_result ns = tgpio_cycles_to_ns(art);

	return ns.status < 0 ? 0 : ns.ns;
}

static u64 tgpio_abs_s64(s64 value)
{
	if (value < 0)
		return (u64)(-(value + 1)) + 1;

	return value;
}

static struct tgpio_s64_result tgpio_add_s64(s64 lhs, s64 rhs)
{
	if (rhs > 0 && lhs > S64_MAX - rhs)
		return tgpio_err_s64(TGPIO_E_RANGE);
	if (rhs < 0 && lhs < S64_MIN - rhs)
		return tgpio_err_s64(TGPIO_E_RANGE);

	return tgpio_ok_s64(lhs + rhs);
}

static struct tgpio_s64_result
tgpio_cycles_delta_to_ns(s64 cycles, u64 cycles_per_sec, bool round_nearest)
{
	struct tgpio_u64_result ns;

	ns = round_nearest ?
		     tgpio_rescale_nearest(tgpio_abs_s64(cycles),
					   cycles_per_sec, NSEC_PER_SEC) :
		     tgpio_rescale(tgpio_abs_s64(cycles), cycles_per_sec,
				   NSEC_PER_SEC);
	if (ns.status < 0)
		return tgpio_err_s64(ns.status);
	if (ns.val > S64_MAX)
		return tgpio_err_s64(TGPIO_E_RANGE);

	return tgpio_ok_s64(cycles < 0 ? -(s64)ns.val : (s64)ns.val);
}

static struct tgpio_s64_result
tgpio_ns_delta_to_cycles(s64 ns, u64 cycles_per_sec, bool round_nearest)
{
	struct tgpio_u64_result cycles;

	cycles = round_nearest ?
			 tgpio_rescale_nearest(tgpio_abs_s64(ns),
					       NSEC_PER_SEC,
					       cycles_per_sec) :
			 tgpio_rescale(tgpio_abs_s64(ns), NSEC_PER_SEC,
				       cycles_per_sec);
	if (cycles.status < 0)
		return tgpio_err_s64(cycles.status);
	if (cycles.val > S64_MAX)
		return tgpio_err_s64(TGPIO_E_RANGE);

	return tgpio_ok_s64(ns < 0 ? -(s64)cycles.val : (s64)cycles.val);
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
	u64 abs_ns = tgpio_abs_s64(ns);
	u64 abs_ppb = tgpio_abs_s64(ppb);
	u64 seconds;
	u64 rem;
	u64 correction;

	if (!ns || !ppb)
		return 0;

	seconds = div64_u64_rem(abs_ns, NSEC_PER_SEC, &rem);
	correction = seconds * abs_ppb + div64_u64(rem * abs_ppb, NSEC_PER_SEC);

	if (correction > S64_MAX)
		correction = S64_MAX;

	return ((ns < 0) ^ (ppb < 0)) ? -(s64)correction : (s64)correction;
}

static struct tgpio_s64_result tgpio_phc_base_to_adjusted_delta(s64 base_ns,
								s64 ppb)
{
	return tgpio_add_s64(base_ns, tgpio_apply_ppb_to_ns(base_ns, ppb));
}

static struct tgpio_s64_result tgpio_phc_adjusted_to_base_delta(s64 adjusted_ns,
								s64 ppb)
{
	u64 abs_adjusted = tgpio_abs_s64(adjusted_ns);
	s64 denominator_s64;
	u64 denominator;
	u64 quotient;
	u64 rem;
	u64 abs_base;

	denominator_s64 = (s64)NSEC_PER_SEC + ppb;
	if (denominator_s64 <= 0)
		return tgpio_err_s64(TGPIO_E_RANGE);
	denominator = denominator_s64;

	quotient = div64_u64_rem(abs_adjusted, denominator, &rem);
	if (quotient > div64_u64(S64_MAX, NSEC_PER_SEC))
		return tgpio_err_s64(TGPIO_E_RANGE);

	abs_base = quotient * NSEC_PER_SEC +
		   div64_u64(rem * NSEC_PER_SEC, denominator);
	if (abs_base > S64_MAX)
		return tgpio_err_s64(TGPIO_E_RANGE);

	return tgpio_ok_s64(adjusted_ns < 0 ? -(s64)abs_base : (s64)abs_base);
}

static struct tgpio_phc_params
tgpio_phc_params_make(u64 anchor_art, s64 anchor_ns, u64 base_art_hz,
		      long scaled_ppm)
{
	return (struct tgpio_phc_params){
		.anchor_art = anchor_art,
		.anchor_ns = anchor_ns,
		.base_art_hz = base_art_hz,
		.scaled_ppm = scaled_ppm,
	};
}

static struct tgpio_s64_result tgpio_phc_art_to_ns(struct tgpio_phc_params phc,
						   u64 art)
{
	struct tgpio_s64_result base;
	struct tgpio_s64_result adjusted;
	s64 cycles_delta;
	s64 ppb;

	if (art >= phc.anchor_art) {
		if (art - phc.anchor_art > S64_MAX)
			return tgpio_err_s64(TGPIO_E_RANGE);
		cycles_delta = (s64)(art - phc.anchor_art);
	} else {
		if (phc.anchor_art - art > S64_MAX)
			return tgpio_err_s64(TGPIO_E_RANGE);
		cycles_delta = -(s64)(phc.anchor_art - art);
	}
	base = tgpio_cycles_delta_to_ns(cycles_delta, phc.base_art_hz, false);
	if (base.status < 0)
		return base;

	ppb = tgpio_scaled_ppm_to_ppb(phc.scaled_ppm);
	adjusted = tgpio_phc_base_to_adjusted_delta(base.val, ppb);
	if (adjusted.status < 0)
		return adjusted;

	return tgpio_add_s64(phc.anchor_ns, adjusted.val);
}

static struct tgpio_u64_result tgpio_phc_ns_to_art(struct tgpio_phc_params phc,
						   s64 phc_ns)
{
	struct tgpio_s64_result phc_delta;
	struct tgpio_s64_result base_delta;
	struct tgpio_s64_result cycles;
	s64 ppb;

	phc_delta = tgpio_add_s64(phc_ns, -phc.anchor_ns);
	if (phc_delta.status < 0)
		return tgpio_err_u64(phc_delta.status);

	ppb = tgpio_scaled_ppm_to_ppb(phc.scaled_ppm);
	base_delta = tgpio_phc_adjusted_to_base_delta(phc_delta.val, ppb);
	if (base_delta.status < 0)
		return tgpio_err_u64(base_delta.status);

	cycles = tgpio_ns_delta_to_cycles(base_delta.val, phc.base_art_hz,
					  true);
	if (cycles.status < 0)
		return tgpio_err_u64(cycles.status);

	if (cycles.val >= 0) {
		u64 art = phc.anchor_art + cycles.val;

		if (art < phc.anchor_art)
			return tgpio_err_u64(TGPIO_E_RANGE);
		return tgpio_ok_u64(art);
	}

	if (tgpio_abs_s64(cycles.val) > phc.anchor_art)
		return tgpio_err_u64(TGPIO_E_RANGE);
	return tgpio_ok_u64(phc.anchor_art - tgpio_abs_s64(cycles.val));
}

static struct tgpio_u64_result
tgpio_phc_delta_to_real_ns(struct tgpio_phc_params phc, s64 phc_delta_ns)
{
	struct tgpio_s64_result base;
	s64 ppb;

	if (phc_delta_ns <= 0)
		return tgpio_ok_u64(0);

	ppb = tgpio_scaled_ppm_to_ppb(phc.scaled_ppm);
	base = tgpio_phc_adjusted_to_base_delta(phc_delta_ns, ppb);
	if (base.status < 0 || base.val < 0)
		return tgpio_err_u64(TGPIO_E_RANGE);

	return tgpio_ok_u64((u64)base.val);
}

static struct tgpio_u64_result
tgpio_phc_delta_to_art_cycles(struct tgpio_phc_params phc, s64 phc_delta_ns)
{
	struct tgpio_s64_result base;
	struct tgpio_s64_result cycles;
	s64 ppb;

	if (phc_delta_ns <= 0)
		return tgpio_ok_u64(0);

	ppb = tgpio_scaled_ppm_to_ppb(phc.scaled_ppm);
	base = tgpio_phc_adjusted_to_base_delta(phc_delta_ns, ppb);
	if (base.status < 0 || base.val < 0)
		return tgpio_err_u64(TGPIO_E_RANGE);

	cycles = tgpio_ns_delta_to_cycles(base.val, phc.base_art_hz, true);
	if (cycles.status < 0 || cycles.val < 0)
		return tgpio_err_u64(cycles.status < 0 ? cycles.status :
						      TGPIO_E_RANGE);

	return tgpio_ok_u64((u64)cycles.val);
}

static struct tgpio_ns_result
tgpio_phc_art_cycles_to_delta_ns(struct tgpio_phc_params phc, u64 cycles)
{
	struct tgpio_s64_result base;
	struct tgpio_s64_result adjusted;
	s64 ppb;

	if (cycles > S64_MAX)
		return (struct tgpio_ns_result){ .status = TGPIO_E_RANGE };

	base = tgpio_cycles_delta_to_ns((s64)cycles, phc.base_art_hz, true);
	if (base.status < 0)
		return (struct tgpio_ns_result){ .status = base.status };

	ppb = tgpio_scaled_ppm_to_ppb(phc.scaled_ppm);
	adjusted = tgpio_phc_base_to_adjusted_delta(base.val, ppb);
	if (adjusted.status < 0 || adjusted.val < 0)
		return (struct tgpio_ns_result){ .status = TGPIO_E_RANGE };

	return (struct tgpio_ns_result){ .status = TGPIO_OK,
					 .ns = (u64)adjusted.val };
}

static struct tgpio_u64_result
tgpio_ptp_time_to_ns(const struct ptp_clock_time *time)
{
	u64 sec = time->sec;

	if (time->sec < 0 || time->nsec >= NSEC_PER_SEC)
		return tgpio_err_u64(TGPIO_E_INVAL);

	if (time->sec > div64_u64(U64_MAX, NSEC_PER_SEC))
		return tgpio_err_u64(TGPIO_E_RANGE);

	return tgpio_ok_u64(sec * NSEC_PER_SEC + time->nsec);
}

static struct tgpio_s64_result
tgpio_ptp_time_to_s64_ns(const struct ptp_clock_time *time)
{
	if (time->sec < 0 || time->nsec >= NSEC_PER_SEC)
		return tgpio_err_s64(TGPIO_E_INVAL);

	if (time->sec > div64_s64(S64_MAX, NSEC_PER_SEC))
		return tgpio_err_s64(TGPIO_E_RANGE);

	return tgpio_ok_s64(time->sec * NSEC_PER_SEC + time->nsec);
}

static inline u32 tgpio_readl(struct tgpio_mmio_block *mmio_block, u32 offset)
{
	return readl(mmio_block->regs + offset);
}

static inline void tgpio_writel(struct tgpio_mmio_block *mmio_block, u32 offset,
				u32 value)
{
	writel(value, mmio_block->regs + offset);
}

static inline u32 tgpio_read_ctl(struct tgpio_mmio_block *mmio_block)
{
	return tgpio_readl(mmio_block, TGPIOCTL);
}

static inline void tgpio_write_ctl(struct tgpio_mmio_block *mmio_block,
				   u32 value)
{
	tgpio_writel(mmio_block, TGPIOCTL, value);
}

static inline void tgpio_write_compv(struct tgpio_mmio_block *mmio_block,
				     u64 value)
{
	tgpio_writel(mmio_block, TGPIOCOMPV63_32, upper_32_bits(value));
	tgpio_writel(mmio_block, TGPIOCOMPV31_0, lower_32_bits(value));
}

static inline void tgpio_write_piv(struct tgpio_mmio_block *mmio_block,
				   u64 value)
{
	tgpio_writel(mmio_block, TGPIOPIV63_32, upper_32_bits(value));
	tgpio_writel(mmio_block, TGPIOPIV31_0, lower_32_bits(value));
}

/*
 * COMPV reads back live: hardware periodic mode advances it by PIV on each
 * generated edge (verified: COMPV == TCV + PIV on running blocks), so the
 * value can change between the two 32-bit reads. Retry until the high word
 * is stable across the low-word read.
 */
static u64 tgpio_read_compv(struct tgpio_mmio_block *mmio_block)
{
	u32 hi = tgpio_readl(mmio_block, TGPIOCOMPV63_32);
	u32 lo;
	u32 prev_hi;

	do {
		prev_hi = hi;
		lo = tgpio_readl(mmio_block, TGPIOCOMPV31_0);
		hi = tgpio_readl(mmio_block, TGPIOCOMPV63_32);
	} while (hi != prev_hi);

	return ((u64)hi << 32) | lo;
}

/*
 * output_work-only: fold the toggles generated since the last checkpoint
 * into the tracked level flop. COMPV advances by exactly PIV per generated
 * edge, and the checkpoint is refreshed at every PIV change and COMPV
 * rewrite, so the distance is always an exact multiple of the current PIV.
 * The flop itself is write-only hardware state: it holds the level of the
 * last toggle, survives disable, and has no reset or readback, so this
 * software mirror is the only source of polarity truth.
 */
static void tgpio_hw_flop_fold(struct tgpio_mmio_block *mmio_block)
{
	u64 compv;
	u64 events;

	if (mmio_block->output_phase != TGPIO_OUTPUT_HARDWARE_PERIODIC ||
	    !mmio_block->output_hw_piv)
		return;

	compv = tgpio_read_compv(mmio_block);
	if (compv < mmio_block->output_hw_ckpt_compv)
		return;

	events = div64_u64(compv - mmio_block->output_hw_ckpt_compv,
			   mmio_block->output_hw_piv);
	if (events & 1)
		mmio_block->output_hw_flop_high =
			!mmio_block->output_hw_flop_high;
	mmio_block->output_hw_ckpt_compv = compv;
}

static struct tgpio_capture
tgpio_read_capture(struct tgpio_mmio_block *mmio_block)
{
	/* Reading TCV low latches the capture timestamp and event count. */
	u64 tcv_lo = tgpio_readl(mmio_block, TGPIOTCV31_0);
	u64 count_hi = tgpio_readl(mmio_block, TGPIOECCV63_32);
	u64 count_lo = tgpio_readl(mmio_block, TGPIOECCV31_0);
	u64 tcv_hi = tgpio_readl(mmio_block, TGPIOTCV63_32);

	return (struct tgpio_capture){
		.event_count = (count_hi << 32) | count_lo,
		.art_cycles = (tcv_hi << 32) | tcv_lo,
	};
}

static struct tgpio_u64_result tgpio_detect_cpuid_art_frequency(void)
{
#ifdef CONFIG_X86
	u32 max_leaf;
	u32 eax;
	u32 ebx;
	u32 ecx;
	u32 edx;

	cpuid(0, &max_leaf, &ebx, &ecx, &edx);
	if (max_leaf < TGPIO_CPUID_ART_LEAF)
		return tgpio_err_u64(TGPIO_E_NODEV);

	cpuid_count(TGPIO_CPUID_ART_LEAF, 0, &eax, &ebx, &ecx, &edx);
	if (!eax || !ebx)
		return tgpio_err_u64(TGPIO_E_NODEV);

	tsc_art_numerator = ebx;
	tsc_art_denominator = eax;
	if (!ecx)
		return tgpio_err_u64(TGPIO_E_NODEV);

	return tgpio_ok_u64(ecx);
#else
	return tgpio_err_u64(TGPIO_E_NODEV);
#endif
}

static int tgpio_resolve_art_frequency(void)
{
	struct tgpio_u64_result detected = tgpio_detect_cpuid_art_frequency();

	if (art_frequency) {
		pr_info("using manual ART frequency %lu Hz\n", art_frequency);
		if (tsc_art_numerator && tsc_art_denominator)
			pr_info("detected CPUID leaf %#x TSC/ART ratio %u/%u\n",
				TGPIO_CPUID_ART_LEAF, tsc_art_numerator,
				tsc_art_denominator);
		return 0;
	}

	if (detected.status >= 0) {
		art_frequency = detected.val;
		pr_info("auto-detected ART frequency %lu Hz from CPUID leaf %#x; TSC/ART ratio %u/%u\n",
			art_frequency, TGPIO_CPUID_ART_LEAF, tsc_art_numerator,
			tsc_art_denominator);
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

static void tgpio_probe_art_parameters_for_status(void)
{
	struct tgpio_u64_result detected = tgpio_detect_cpuid_art_frequency();

	if (detected.status >= 0) {
		if (!art_frequency)
			art_frequency = detected.val;
		pr_info("detected CPUID leaf %#x ART frequency %llu Hz; TSC/ART ratio %u/%u\n",
			TGPIO_CPUID_ART_LEAF, detected.val, tsc_art_numerator,
			tsc_art_denominator);
	} else if (tsc_art_numerator && tsc_art_denominator) {
		pr_info("detected CPUID leaf %#x TSC/ART ratio %u/%u; ART frequency not reported\n",
			TGPIO_CPUID_ART_LEAF, tsc_art_numerator,
			tsc_art_denominator);
	}
}

static struct tgpio_u64_result tgpio_get_current_art(void)
{
	struct system_time_snapshot snapshot;
	u64 art;

	tgpio_get_realtime_snapshot(&snapshot);
	if (tgpio_snapshot_art_cycles(&snapshot, &art))
		return tgpio_ok_u64(art);

	/*
	 * No ART cycles in the timekeeping snapshot on this kernel. The
	 * inverse realtime mapping is exact for the current instant even
	 * while NTP disciplines CLOCK_REALTIME (rate changes tilt the slope,
	 * not the current point), so it is a safe fallback for "now" reads.
	 */
	if (!ktime_real_to_base_clock(ktime_get_real(), CSID_X86_ART, &art))
		return tgpio_err_u64(TGPIO_E_NODEV);
	return tgpio_ok_u64(art);
}

static struct tgpio_u64_result tgpio_realtime_delta_to_art_cycles(u64 delta_ns)
{
	ktime_t start;
	ktime_t end;
	s64 start_ns;
	struct tgpio_s64_result end_ns;
	u64 start_art;
	u64 end_art;

	if (!delta_ns || delta_ns > S64_MAX)
		return tgpio_err_u64(TGPIO_E_RANGE);

	start = ktime_get_real();
	start_ns = ktime_to_ns(start);
	if (start_ns < 0)
		return tgpio_err_u64(TGPIO_E_RANGE);

	end_ns = tgpio_add_s64(start_ns, (s64)delta_ns);
	if (end_ns.status < 0)
		return tgpio_err_u64(end_ns.status);

	end = ns_to_ktime(end_ns.val);
	if (!ktime_real_to_base_clock(start, CSID_X86_ART, &start_art) ||
	    !ktime_real_to_base_clock(end, CSID_X86_ART, &end_art))
		return tgpio_err_u64(TGPIO_E_NODEV);

	if (end_art <= start_art)
		return tgpio_err_u64(TGPIO_E_RANGE);

	return tgpio_ok_u64(end_art - start_art);
}

static struct tgpio_ns_result tgpio_art_cycles_to_realtime_delta_ns(u64 cycles)
{
	struct tgpio_u64_result cycles_per_sec =
		tgpio_realtime_delta_to_art_cycles(NSEC_PER_SEC);
	struct tgpio_u64_result ns;

	if (cycles_per_sec.status < 0)
		return (struct tgpio_ns_result){ .status = cycles_per_sec.status };

	ns = tgpio_rescale_nearest(cycles, cycles_per_sec.val, NSEC_PER_SEC);
	return (struct tgpio_ns_result){ .status = ns.status, .ns = ns.val };
}

static struct tgpio_phc_params tgpio_phc_params_get(struct tgpio_device *dev)
{
	struct tgpio_phc_params phc;
	unsigned int seq;

	do {
		seq = read_seqbegin(&dev->phc_seqlock);
		phc = dev->phc;
	} while (read_seqretry(&dev->phc_seqlock, seq));

	return phc;
}

static struct tgpio_s64_result
tgpio_clock_phc_art_to_ns(struct tgpio_device *dev, u64 art)
{
	return tgpio_phc_art_to_ns(tgpio_phc_params_get(dev), art);
}

static struct tgpio_s64_result tgpio_clock_phc_now_ns(struct tgpio_device *dev)
{
	struct tgpio_u64_result art = tgpio_get_current_art();

	if (art.status < 0)
		return tgpio_err_s64(art.status);

	return tgpio_clock_phc_art_to_ns(dev, art.val);
}

static struct tgpio_s64_result tgpio_clock_now_ns(struct tgpio_device *dev)
{
	if (clock_mode == TGPIO_CLOCK_PHC)
		return tgpio_clock_phc_now_ns(dev);

	return tgpio_ok_s64(ktime_get_real_ns());
}

static struct tgpio_u64_result tgpio_clock_ns_to_art(struct tgpio_device *dev,
						     s64 ns)
{
	u64 art;

	if (clock_mode == TGPIO_CLOCK_PHC)
		return tgpio_phc_ns_to_art(tgpio_phc_params_get(dev), ns);

	if (!ktime_real_to_base_clock(ns_to_ktime(ns), CSID_X86_ART, &art))
		return tgpio_err_u64(TGPIO_E_NODEV);

	return tgpio_ok_u64(art);
}

static struct tgpio_u64_result
tgpio_clock_delta_to_real_ns(struct tgpio_device *dev, s64 clock_delta_ns)
{
	if (clock_mode == TGPIO_CLOCK_PHC)
		return tgpio_phc_delta_to_real_ns(tgpio_phc_params_get(dev),
						  clock_delta_ns);

	if (clock_delta_ns <= 0)
		return tgpio_ok_u64(0);

	return tgpio_ok_u64(clock_delta_ns);
}

static struct tgpio_u64_result
tgpio_clock_delta_to_art_cycles(struct tgpio_device *dev, s64 clock_delta_ns)
{
	struct tgpio_u64_result real;
	struct tgpio_u64_result cycles;

	if (clock_mode == TGPIO_CLOCK_PHC) {
		cycles = tgpio_phc_delta_to_art_cycles(
			tgpio_phc_params_get(dev), clock_delta_ns);
		if (cycles.status < 0)
			return cycles;
		if (!cycles.val)
			return tgpio_err_u64(TGPIO_E_RANGE);
		return cycles;
	}

	real = tgpio_clock_delta_to_real_ns(dev, clock_delta_ns);
	if (real.status < 0 || real.val > S64_MAX)
		return tgpio_err_u64(real.status < 0 ? real.status :
							TGPIO_E_RANGE);

	cycles = tgpio_realtime_delta_to_art_cycles(real.val);
	if (cycles.status < 0)
		return cycles;
	if (!cycles.val)
		return tgpio_err_u64(TGPIO_E_RANGE);

	return cycles;
}

static struct tgpio_ns_result
tgpio_clock_art_cycles_to_delta_ns(struct tgpio_device *dev, u64 cycles)
{
	if (clock_mode == TGPIO_CLOCK_PHC)
		return tgpio_phc_art_cycles_to_delta_ns(
			tgpio_phc_params_get(dev), cycles);

	return tgpio_art_cycles_to_realtime_delta_ns(cycles);
}

static struct tgpio_output_quantization
tgpio_output_quantization_err(enum tgpio_status status)
{
	return (struct tgpio_output_quantization){ .status = status };
}

static struct tgpio_output_quantization
tgpio_output_quantization_get(struct tgpio_device *dev, u64 period_ns,
			      u64 half_period_ns)
{
	struct tgpio_u64_result art_half;
	struct tgpio_ns_result actual_half;
	struct tgpio_s64_result period_error;
	s64 half_error;
	s64 split_error = period_ns & 1 ? -1 : 0;

	art_half = tgpio_clock_delta_to_art_cycles(dev, half_period_ns);
	if (art_half.status < 0 || !art_half.val)
		return tgpio_output_quantization_err(
			art_half.status < 0 ? art_half.status : TGPIO_E_RANGE);

	actual_half = tgpio_clock_art_cycles_to_delta_ns(dev, art_half.val);
	if (actual_half.status < 0 || actual_half.ns > S64_MAX)
		return tgpio_output_quantization_err(
			actual_half.status < 0 ? actual_half.status :
						 TGPIO_E_RANGE);

	half_error = (s64)actual_half.ns - (s64)half_period_ns;
	period_error = tgpio_add_s64(half_error, half_error);
	if (period_error.status < 0)
		return tgpio_output_quantization_err(period_error.status);

	period_error = tgpio_add_s64(period_error.val, split_error);
	if (period_error.status < 0)
		return tgpio_output_quantization_err(period_error.status);

	return (struct tgpio_output_quantization){
		.status = TGPIO_OK,
		.clock_half_period_ns = half_period_ns,
		.art_half_period_cycles = art_half.val,
		.actual_half_period_ns = actual_half.ns,
		.actual_period_ns = actual_half.ns * 2,
		.half_period_error_ns = half_error,
		.period_error_ns = period_error.val,
		.period_split_error_ns = split_error,
	};
}

static int tgpio_phc_init_clock(struct tgpio_device *dev)
{
	struct tgpio_u64_result base_art_hz;
	struct system_time_snapshot snapshot;
	ktime_t realtime;
	u64 art;

	tgpio_get_realtime_snapshot(&snapshot);
	if (tgpio_snapshot_art_cycles(&snapshot, &art)) {
		realtime = tgpio_snapshot_realtime(snapshot);
	} else {
		pr_warn("timekeeping snapshot lacks ART cycles; PHC anchors via CLOCK_REALTIME inversion\n");
		realtime = ktime_get_real();
		if (!ktime_real_to_base_clock(realtime, CSID_X86_ART, &art))
			return -ENODEV;
	}

	if (art_frequency) {
		base_art_hz = tgpio_ok_u64(art_frequency);
		pr_info("PHC uses manual ART frequency %llu Hz\n",
			base_art_hz.val);
	} else {
		base_art_hz =
			tgpio_realtime_delta_to_art_cycles(NSEC_PER_SEC);
		if (base_art_hz.status < 0)
			return base_art_hz.status;
		pr_info("PHC calibrated ART base frequency %llu Hz from timekeeper\n",
			base_art_hz.val);
	}

	dev->phc = tgpio_phc_params_make(art, ktime_to_ns(realtime),
					 base_art_hz.val, 0);
	return 0;
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

static struct tgpio_u64_result
tgpio_art_to_realtime_ns(struct tgpio_mmio_block *mmio_block, u64 art)
{
	struct tgpio_crosststamp_ctx ctx = {
		.art_cycles = art,
	};
	struct system_device_crosststamp xtstamp = {};
	struct system_time_snapshot *history = NULL;

	if (mmio_block->crosststamp_snapshot_state == TGPIO_SNAPSHOT_PRESENT)
		history = &mmio_block->crosststamp_snapshot;

	if (get_device_system_crosststamp(tgpio_get_crosststamp, &ctx, history,
					  &xtstamp))
		return tgpio_err_u64(TGPIO_E_NODEV);

	return tgpio_ok_u64(ktime_to_ns(tgpio_xtstamp_realtime(xtstamp)));
}

static struct tgpio_crosststamp_capture tgpio_take_crosststamp(void)
{
	struct system_time_snapshot snapshot;

	if (!hardware_timestamps || clock_mode != TGPIO_CLOCK_REALTIME ||
	    timestamp_mode != TGPIO_TIMESTAMP_REALTIME)
		return (struct tgpio_crosststamp_capture){
			.state = TGPIO_SNAPSHOT_NONE
		};

	tgpio_get_realtime_snapshot(&snapshot);
	return (struct tgpio_crosststamp_capture){
		.state = TGPIO_SNAPSHOT_PRESENT,
		.snapshot = snapshot,
	};
}

static enum tgpio_support
tgpio_mmio_block_supports_func(struct tgpio_device *dev,
			       unsigned int block_index,
			       enum ptp_pin_function func)
{
	if (block_index >= dev->n_ptp_pins)
		return TGPIO_UNSUPPORTED;

	switch (func) {
	case PTP_PF_EXTTS:
		return dev->mmio_blocks[block_index].mode == TGPIO_MODE_INPUT ?
			       TGPIO_SUPPORTED :
			       TGPIO_UNSUPPORTED;
	case PTP_PF_PEROUT:
		return dev->mmio_blocks[block_index].mode == TGPIO_MODE_OUTPUT ?
			       TGPIO_SUPPORTED :
			       TGPIO_UNSUPPORTED;
	default:
		return TGPIO_UNSUPPORTED;
	}
}

static int tgpio_find_mmio_block_for_channel(struct tgpio_device *dev,
					     enum ptp_pin_function func,
					     unsigned int channel)
{
	unsigned int i;

	if (channel >= dev->n_ptp_pins)
		return -EINVAL;

	for (i = 0; i < dev->n_ptp_pins; i++) {
		if (tgpio_mmio_block_supports_func(dev, i, func) ==
		    TGPIO_UNSUPPORTED)
			continue;

		if (dev->pin_config[i].func == func &&
		    dev->pin_config[i].chan == channel)
			return i;
	}

	if (tgpio_mmio_block_supports_func(dev, channel, func) ==
	    TGPIO_SUPPORTED)
		return channel;

	return -EOPNOTSUPP;
}

static unsigned int tgpio_channel_for_mmio_block(struct tgpio_device *dev,
						 unsigned int block_index,
						 enum ptp_pin_function func)
{
	if (block_index < dev->n_ptp_pins &&
	    dev->pin_config[block_index].func == func)
		return dev->pin_config[block_index].chan;

	return block_index;
}

static void tgpio_log_input_event(struct tgpio_mmio_block *mmio_block,
				  unsigned int channel, u64 event_count,
				  u64 event_delta, u64 art_cycles,
				  s64 timestamp_ns, bool fallback)
{
	if (!READ_ONCE(activity_log))
		return;

	pr_info("activity=input_event block=%u channel=%u edge=%s event_count=%llu event_delta=%llu art=%llu timestamp_ns=%lld clock_mode=%s timestamp_mode=%s hardware_timestamps=%c fallback=%c\n",
		mmio_block->index, channel,
		tgpio_edge_bits_name(mmio_block->input_applied),
		event_count, event_delta, art_cycles, timestamp_ns,
		tgpio_clock_mode_name(clock_mode),
		tgpio_timestamp_mode_name(timestamp_mode),
		hardware_timestamps ? 'Y' : 'N', fallback ? 'Y' : 'N');
}

static void tgpio_emit_event(struct tgpio_device *dev,
			     struct tgpio_mmio_block *mmio_block,
			     unsigned int index, u64 art_cycles,
			     u64 event_count, u64 event_delta)
{
	struct ptp_clock_event event = {
		.type = PTP_CLOCK_EXTTS,
		.index = index,
	};

	struct tgpio_s64_result ts;
	bool fallback = false;

	if (!dev->ptp_clock)
		return;

	if (!hardware_timestamps) {
		ts = tgpio_clock_now_ns(dev);
		if (ts.status < 0 || ts.val < 0)
			ts = tgpio_ok_s64(ktime_get_real_ns());
	} else if (clock_mode == TGPIO_CLOCK_PHC) {
		ts = tgpio_clock_phc_art_to_ns(dev, art_cycles);
		if (ts.status < 0 || ts.val < 0) {
			trace_tgpio_timestamp_fallback(mmio_block->index,
						       art_cycles);
			atomic64_inc(&dev->stats.fallbacks);
			fallback = true;
			pr_warn_ratelimited(
				"ART->PHC convert failed; used poll time\n");
			ts = tgpio_clock_now_ns(dev);
			if (ts.status < 0 || ts.val < 0)
				ts = tgpio_ok_s64(ktime_get_real_ns());
		}
	} else if (timestamp_mode == TGPIO_TIMESTAMP_REALTIME) {
		struct tgpio_u64_result rt =
			tgpio_art_to_realtime_ns(mmio_block, art_cycles);

		if (rt.status < 0) {
			trace_tgpio_timestamp_fallback(mmio_block->index,
						       art_cycles);
			atomic64_inc(&dev->stats.fallbacks);
			fallback = true;
			pr_warn_ratelimited(
				"ART->CLOCK_REALTIME convert failed; used poll time\n");
			ts = tgpio_ok_s64(ktime_get_real_ns());
		} else {
			ts = tgpio_ok_s64(rt.val);
		}
	} else {
		ts = tgpio_ok_s64(tgpio_art_to_ns(art_cycles));
	}

	event.timestamp = ts.val;
	atomic64_inc(&dev->stats.events);
	tgpio_log_input_event(mmio_block, index, event_count, event_delta,
			      art_cycles, ts.val, fallback);
	ptp_clock_event(dev->ptp_clock, &event);
}

static unsigned long tgpio_poll_interval_jiffies(void)
{
	/*
	 * poll_ms is runtime-writable; clamp to TGPIO_MIN_POLL_MS so a value of
	 * 0 never reschedules with no delay and busy-spins the workqueue.
	 */
	return msecs_to_jiffies(max(poll_ms, TGPIO_MIN_POLL_MS));
}

/* Packed "desired" input config: TGPIO_INPUT_DESIRED_ON plus the edge in TGPIOCTL_EP. */
static int tgpio_input_desired(enum tgpio_capture_state state, u32 edge_bits)
{
	return (state == TGPIO_CAPTURE_ON ? TGPIO_INPUT_DESIRED_ON : 0) |
	       edge_bits;
}

static enum tgpio_capture_state tgpio_input_desired_state(int desired)
{
	return (desired & TGPIO_INPUT_DESIRED_ON) ? TGPIO_CAPTURE_ON :
						    TGPIO_CAPTURE_OFF;
}

static u32 tgpio_input_desired_edge(int desired)
{
	return desired & TGPIOCTL_EP;
}

/* Reconciler-only: bring an input block's hardware to the desired encoding. */
static void tgpio_apply_input(struct tgpio_mmio_block *mmio_block, int desired)
{
	u32 ctrl = tgpio_ctl_without(tgpio_read_ctl(mmio_block), TGPIOCTL_EN);

	tgpio_write_ctl(mmio_block, ctrl);

	if (tgpio_input_desired_state(desired) == TGPIO_CAPTURE_OFF) {
		mmio_block->crosststamp_snapshot_state = TGPIO_SNAPSHOT_NONE;
		return;
	}

	ctrl = tgpio_ctl_with_edge(ctrl, tgpio_input_desired_edge(desired));
	ctrl = tgpio_ctl_with(ctrl, TGPIOCTL_DIR);
	tgpio_write_ctl(mmio_block, ctrl);

	/* Baseline so the first poll doesn't emit a stale event. */
	mmio_block->last_event_count =
		tgpio_read_capture(mmio_block).event_count;

	tgpio_write_ctl(mmio_block, tgpio_ctl_with(ctrl, TGPIOCTL_EN));
}

static void tgpio_poll_work(struct work_struct *work)
{
	struct tgpio_device *dev = container_of(to_delayed_work(work),
						struct tgpio_device, poll_work);
	enum tgpio_capture_state active = TGPIO_CAPTURE_OFF;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		struct tgpio_mmio_block *mmio_block = &dev->mmio_blocks[i];
		struct tgpio_crosststamp_capture snap;
		struct tgpio_capture cap;
		int desired;

		if (mmio_block->mode != TGPIO_MODE_INPUT)
			continue;

		desired = atomic_read(&mmio_block->input_desired);
		if (desired != mmio_block->input_applied) {
			tgpio_apply_input(mmio_block, desired);
			mmio_block->input_applied = desired;
		}

		if (tgpio_input_desired_state(desired) == TGPIO_CAPTURE_OFF)
			continue;

		active = TGPIO_CAPTURE_ON;
		snap = tgpio_take_crosststamp();
		cap = tgpio_read_capture(mmio_block);

		if (cap.event_count != mmio_block->last_event_count) {
			unsigned int channel;
			u64 event_delta = cap.event_count -
					  mmio_block->last_event_count;

			mmio_block->last_event_count = cap.event_count;
			channel = tgpio_channel_for_mmio_block(dev, i,
							       PTP_PF_EXTTS);
			tgpio_emit_event(dev, mmio_block, channel,
					 cap.art_cycles, cap.event_count,
					 event_delta);
		}

		mmio_block->crosststamp_snapshot = snap.snapshot;
		mmio_block->crosststamp_snapshot_state = snap.state;
	}

	if (active == TGPIO_CAPTURE_ON)
		schedule_delayed_work(&dev->poll_work,
				      tgpio_poll_interval_jiffies());
}

static int tgpio_config_input(struct tgpio_device *dev, unsigned int channel,
			      unsigned int flags, int on)
{
	struct tgpio_mmio_block *mmio_block;
	int block_index;
	u32 edge_bits = 0;

	block_index =
		tgpio_find_mmio_block_for_channel(dev, PTP_PF_EXTTS, channel);
	if (block_index < 0)
		return block_index;

	mmio_block = &dev->mmio_blocks[block_index];
	if (mmio_block->mode != TGPIO_MODE_INPUT || !mmio_block->regs)
		return -EOPNOTSUPP;

	if (on)
		edge_bits = tgpio_input_edge_bits(mmio_block, flags);

	atomic_set(&mmio_block->input_desired,
		   on ? tgpio_input_desired(TGPIO_CAPTURE_ON, edge_bits) :
			tgpio_input_desired(TGPIO_CAPTURE_OFF, 0));
	if (READ_ONCE(activity_log))
		pr_info("activity=input_config block=%u channel=%u state=%s edge=%s flags=%#x\n",
			mmio_block->index, channel, on ? "on" : "off",
			on ? tgpio_edge_bits_name(edge_bits) : "none", flags);
	/*
	 * Reconcile now, not a poll period from now: a deferred disable would keep
	 * delivering EXTTS events until the next tick, and schedule_delayed_work()
	 * won't pull an already-queued poll earlier.
	 */
	mod_delayed_work(system_wq, &dev->poll_work, 0);
	return 0;
}

/*
 * Convert a logical output edge time into the hardware compare value. The
 * calibration offset shifts every programmed edge so a one-shot external
 * measurement (scope, analyzer, reference timestamper) can cancel constant
 * pipeline and clock-comparison delays; the phase readback in
 * tgpio_hw_periodic_phase_get applies the inverse, so the nudge machinery
 * converges a running output onto the shifted position automatically.
 */
static struct tgpio_u64_result
tgpio_clock_ns_to_compare_art(struct tgpio_device *dev, s64 edge_time_ns)
{
	struct tgpio_s64_result shifted =
		tgpio_add_s64(edge_time_ns, READ_ONCE(output_phase_offset_ns));
	struct tgpio_u64_result art;

	if (shifted.status < 0)
		return tgpio_err_u64(shifted.status);

	art = tgpio_clock_ns_to_art(dev, shifted.val);
	if (art.status < 0)
		return art;

	if (art.val > TGPIO_ART_HW_DELAY_CYCLES)
		art.val -= TGPIO_ART_HW_DELAY_CYCLES;

	return art;
}

static enum tgpio_status
tgpio_program_output_edge(struct tgpio_device *dev,
			  struct tgpio_mmio_block *mmio_block,
			  ktime_t edge_time, u32 edge_bits)
{
	struct tgpio_u64_result art =
		tgpio_clock_ns_to_compare_art(dev, ktime_to_ns(edge_time));
	u32 ctrl;

	if (art.status < 0)
		return art.status;

	ctrl = tgpio_read_ctl(mmio_block);
	ctrl &= ~TGPIOCTL_EP;
	ctrl |= edge_bits;
	tgpio_write_ctl(mmio_block, ctrl);

	tgpio_write_compv(mmio_block, art.val);
	return TGPIO_OK;
}

static struct tgpio_output_timing tgpio_output_timing_err(enum tgpio_status e)
{
	return (struct tgpio_output_timing){ .status = e };
}

/*
 * A stale start time must stay on its own period grid: an output re-armed
 * after a PHC step (or started with a past or zero start) fires at
 * start + k * period, never at an arbitrary "now plus lead" phase.
 */
static s64 tgpio_output_align_first_edge(s64 requested_ns, s64 min_first_ns,
					 u64 period_ns)
{
	struct tgpio_s64_result aligned;
	u64 delta;
	u64 periods;
	u64 rem;

	if (requested_ns >= min_first_ns)
		return requested_ns;
	if (!period_ns || period_ns > (u64)S64_MAX || requested_ns < 0)
		return min_first_ns;

	delta = (u64)(min_first_ns - requested_ns);
	periods = div64_u64_rem(delta, period_ns, &rem);
	if (rem)
		periods++;
	if (periods > div64_u64((u64)S64_MAX, period_ns))
		return min_first_ns;

	aligned = tgpio_add_s64(requested_ns, (s64)(periods * period_ns));
	if (aligned.status < 0)
		return min_first_ns;

	return aligned.val;
}

static struct tgpio_output_timing
tgpio_prepare_output_timing(struct tgpio_device *dev, u64 min_interval_ns,
			    u64 period_ns, enum tgpio_output_mode mode,
			    s64 requested_first_edge_ns)
{
	struct tgpio_s64_result now = tgpio_clock_now_ns(dev);
	struct tgpio_s64_result min_first;
	struct tgpio_s64_result prime;
	struct tgpio_s64_result timer_start_ns;
	struct tgpio_s64_result timer_delay_clock;
	struct tgpio_u64_result timer_delay;
	s64 first_edge_ns;
	s64 lead_ns_s64;
	u64 lead_ns;
	ktime_t now_real;
	ktime_t timer_start;

	if (now.status < 0)
		return tgpio_output_timing_err(TGPIO_E_NODEV);
	if (now.val < 0)
		return tgpio_output_timing_err(TGPIO_E_RANGE);

	if (mode == TGPIO_OUTPUT_HARDWARE)
		lead_ns = TGPIO_OUTPUT_SAFE_TIME_NS;
	else
		lead_ns = tgpio_output_lead_time_ns(min_interval_ns);
	if (lead_ns > S64_MAX / 3)
		return tgpio_output_timing_err(TGPIO_E_RANGE);
	lead_ns_s64 = lead_ns;

	min_first = tgpio_add_s64(now.val, 3 * lead_ns_s64);
	if (min_first.status < 0)
		return tgpio_output_timing_err(min_first.status);
	first_edge_ns = tgpio_output_align_first_edge(requested_first_edge_ns,
						      min_first.val, period_ns);

	prime = tgpio_add_s64(first_edge_ns, -2 * lead_ns_s64);
	timer_start_ns = tgpio_add_s64(first_edge_ns, -lead_ns_s64);
	if (prime.status < 0 || timer_start_ns.status < 0)
		return tgpio_output_timing_err(TGPIO_E_RANGE);
	timer_delay_clock = tgpio_add_s64(timer_start_ns.val, -now.val);
	if (timer_delay_clock.status < 0)
		return tgpio_output_timing_err(TGPIO_E_RANGE);

	timer_delay = tgpio_clock_delta_to_real_ns(dev, timer_delay_clock.val);
	if (timer_delay.status < 0)
		return tgpio_output_timing_err(TGPIO_E_NODEV);

	now_real = ktime_get_real();
	timer_start = ktime_add_ns(now_real, timer_delay.val);
	if (ktime_before(timer_start, now_real))
		timer_start = now_real;

	return (struct tgpio_output_timing){
		.status = TGPIO_OK,
		.first_edge_ns = first_edge_ns,
		.prime_edge_ns = prime.val,
		.timer_start = timer_start,
	};
}

static void tgpio_disable_output_hw(struct tgpio_mmio_block *mmio_block)
{
	u32 ctrl = tgpio_read_ctl(mmio_block);

	/* Stop event generation first, then fold the final COMPV state into
	 * the tracked level flop; an edge firing between the fold and the
	 * disable would otherwise be lost from the parity.
	 */
	tgpio_write_ctl(mmio_block, tgpio_ctl_without(ctrl, TGPIOCTL_EN));
	tgpio_hw_flop_fold(mmio_block);
	tgpio_write_compv(mmio_block, 0);
	tgpio_write_piv(mmio_block, 0);
	tgpio_write_ctl(mmio_block,
			tgpio_ctl_without(ctrl, TGPIOCTL_EN | TGPIOCTL_PM));
	mmio_block->output_phase = TGPIO_OUTPUT_TOGGLE;
	mmio_block->output_hw_first_art = 0;
	mmio_block->output_hw_piv = 0;
}

static void tgpio_log_output_hw_periodic(struct tgpio_device *dev,
					 struct tgpio_mmio_block *mmio_block,
					 s64 first_edge_ns, u64 half_period_ns,
					 u64 first_art, u64 half_period_art);
static void tgpio_log_output_phase_rearm(struct tgpio_device *dev,
					 struct tgpio_mmio_block *mmio_block,
					 s64 phase_error_ns);
static void tgpio_log_output_late_push(struct tgpio_device *dev,
				       struct tgpio_mmio_block *mmio_block,
				       u64 periods);

/* output_work-only: switch the block to autonomous hardware periodic output. */
static int tgpio_arm_hardware_periodic(struct tgpio_device *dev,
				       struct tgpio_mmio_block *mmio_block,
				       s64 first_edge_ns, u64 half_period_ns)
{
	struct tgpio_u64_result first_art =
		tgpio_clock_ns_to_compare_art(dev, first_edge_ns);
	struct tgpio_u64_result half_period_art =
		tgpio_clock_delta_to_art_cycles(dev, half_period_ns);
	struct tgpio_u64_result now_art;
	struct tgpio_u64_result margin;
	u32 ctrl;

	if (first_art.status < 0 || half_period_art.status < 0 ||
	    !half_period_art.val || half_period_art.val > U64_MAX / 2)
		return -ENODEV;

	ctrl = tgpio_ctl_without(tgpio_read_ctl(mmio_block), TGPIOCTL_EN);
	tgpio_write_ctl(mmio_block, ctrl);
	tgpio_write_compv(mmio_block, 0);
	tgpio_write_piv(mmio_block, 0);

	ctrl = tgpio_ctl_without(ctrl,
				 TGPIOCTL_DIR | TGPIOCTL_EP | TGPIOCTL_PM);
	tgpio_write_ctl(mmio_block, ctrl);

	/*
	 * The output level flop cannot be loaded or read: it holds the level
	 * of the last generated toggle (it survives disable; EN=1 re-drives
	 * it; EP writes and single-shot compares do nothing). Polarity is
	 * therefore handled by slot selection below: if the tracked flop is
	 * high, the first toggle is a falling edge, so program it on the
	 * half-period slot before a grid point and the rising edges still
	 * land on the requested grid.
	 */
	if (mmio_block->output_hw_flop_high) {
		if (first_art.val > half_period_art.val)
			first_art.val -= half_period_art.val;
		else
			first_art.val += half_period_art.val;
	}

	/*
	 * Late-arm guard: if the compare were already in the past at enable,
	 * hardware would fire the first toggle immediately and invert the
	 * waveform polarity. Push the first edge forward by whole periods
	 * (parity-preserving) until it sits safely ahead of the counter.
	 */
	now_art = tgpio_get_current_art();
	margin = tgpio_clock_delta_to_art_cycles(dev, 2 * NSEC_PER_MSEC);
	if (now_art.status >= 0 && margin.status >= 0 &&
	    now_art.val <= U64_MAX - margin.val) {
		u64 deadline = now_art.val + margin.val;
		u64 period_cycles = 2 * half_period_art.val;

		if (first_art.val < deadline) {
			u64 periods = div64_u64(deadline - first_art.val,
						period_cycles) +
				      1;

			if (periods > div64_u64(U64_MAX - first_art.val,
						period_cycles))
				return -ENODEV;
			first_art.val += periods * period_cycles;
			tgpio_log_output_late_push(dev, mmio_block, periods);
		}
	}

	ctrl = tgpio_ctl_with(ctrl, TGPIOCTL_EP_TOGGLE | TGPIOCTL_PM);
	tgpio_write_ctl(mmio_block, ctrl);

	tgpio_write_piv(mmio_block, half_period_art.val);
	tgpio_write_compv(mmio_block, first_art.val);

	mmio_block->output_high_time_ns = half_period_ns;
	mmio_block->output_low_time_ns = half_period_ns;
	mmio_block->output_next_edge = ns_to_ktime(first_edge_ns);
	mmio_block->output_phase = TGPIO_OUTPUT_HARDWARE_PERIODIC;
	mmio_block->output_hw_first_art = first_art.val;
	mmio_block->output_hw_piv = half_period_art.val;
	mmio_block->output_hw_ckpt_compv = first_art.val;

	tgpio_log_output_hw_periodic(dev, mmio_block, first_edge_ns,
				     half_period_ns, first_art.val,
				     half_period_art.val);
	tgpio_write_ctl(mmio_block, tgpio_ctl_with(ctrl, TGPIOCTL_EN));
	return 0;
}

struct tgpio_hw_phase {
	enum tgpio_status status;
	s64 pending_ns; /* next hardware edge, PHC time */
	s64 error_ns;	/* pending_ns minus nearest half-period grid point */
	bool nudge_safe; /* pending far enough away to rewrite COMPV */
};

/*
 * Phase of the free-running hardware output against the requested period
 * grid, from the live COMPV readback (COMPV always holds the next pending
 * edge; hardware adds PIV to it on each generated edge).
 */
static struct tgpio_hw_phase
tgpio_hw_periodic_phase_get(struct tgpio_device *dev,
			    struct tgpio_mmio_block *mmio_block,
			    s64 grid_start_ns, u64 half_ns)
{
	struct tgpio_s64_result pending;
	struct tgpio_s64_result now;
	u64 compv;
	u64 rem;
	s64 err_ns;

	if (clock_mode != TGPIO_CLOCK_PHC || !half_ns ||
	    half_ns > (u64)S64_MAX / 4 || grid_start_ns < 0)
		return (struct tgpio_hw_phase){ .status = TGPIO_E_INVAL };

	compv = tgpio_read_compv(mmio_block);
	if (!compv || compv > U64_MAX - TGPIO_ART_HW_DELAY_CYCLES)
		return (struct tgpio_hw_phase){ .status = TGPIO_E_RANGE };

	pending = tgpio_clock_phc_art_to_ns(dev,
					    compv + TGPIO_ART_HW_DELAY_CYCLES);
	now = tgpio_clock_now_ns(dev);
	if (pending.status < 0 || now.status < 0 || now.val < 0)
		return (struct tgpio_hw_phase){ .status = TGPIO_E_RANGE };

	/* Back into the logical edge-time domain the grid lives in. */
	pending = tgpio_add_s64(pending.val,
				-READ_ONCE(output_phase_offset_ns));
	if (pending.status < 0 || pending.val < grid_start_ns)
		return (struct tgpio_hw_phase){ .status = TGPIO_E_RANGE };

	/* A pending edge more than two periods out is not a live edge time. */
	if (pending.val < now.val - (s64)half_ns ||
	    pending.val - now.val > (s64)(4 * half_ns))
		return (struct tgpio_hw_phase){ .status = TGPIO_E_RANGE };

	div64_u64_rem((u64)(pending.val - grid_start_ns), half_ns, &rem);
	err_ns = rem >= half_ns - rem ? (s64)rem - (s64)half_ns : (s64)rem;

	return (struct tgpio_hw_phase){
		.status = TGPIO_OK,
		.pending_ns = pending.val,
		.error_ns = err_ns,
		.nudge_safe = pending.val >=
			      now.val + (s64)TGPIO_OUTPUT_SAFE_TIME_NS,
	};
}

/* output_work-only: prime the block and set the initial software-periodic phase. */
static int tgpio_arm_output(struct tgpio_device *dev,
			    struct tgpio_mmio_block *mmio_block,
			    s64 first_edge_ns, s64 prime_edge_ns,
			    u64 high_time_ns, u64 low_time_ns,
			    enum tgpio_output_mode mode)
{
	u32 ctrl = tgpio_ctl_without(tgpio_read_ctl(mmio_block), TGPIOCTL_EN);

	/* Stop events, then fold pending toggles into the tracked level flop
	 * before the registers are cleared; a re-arm that pre-empts a running
	 * waveform (a PHC step, for example) must not lose the parity.
	 */
	tgpio_write_ctl(mmio_block, ctrl);
	tgpio_hw_flop_fold(mmio_block);
	tgpio_write_compv(mmio_block, 0);
	tgpio_write_piv(mmio_block, 0);

	ctrl = tgpio_ctl_without(ctrl,
				 TGPIOCTL_DIR | TGPIOCTL_EP | TGPIOCTL_PM);
	tgpio_write_ctl(mmio_block, ctrl);

	/*
	 * Hardware mode needs no prime edge: single-shot compares never fire
	 * on this hardware, and the level flop cannot be preloaded anyway.
	 * Leave the block disabled (line low) until
	 * tgpio_arm_hardware_periodic picks the compare slot from the
	 * tracked flop and enables toggle mode.
	 */
	if (mode != TGPIO_OUTPUT_HARDWARE &&
	    tgpio_program_output_edge(
		    dev, mmio_block, ns_to_ktime(prime_edge_ns),
		    tgpio_output_edge_bits(TGPIO_EDGE_FALL)) != TGPIO_OK) {
		tgpio_disable_output_hw(mmio_block);
		return -ENODEV;
	}

	mmio_block->output_high_time_ns = high_time_ns;
	mmio_block->output_low_time_ns = low_time_ns;
	mmio_block->output_next_edge = ns_to_ktime(first_edge_ns);
	mmio_block->output_next_edge_type = TGPIO_EDGE_RISE;
	mmio_block->output_phase = mode == TGPIO_OUTPUT_HARDWARE ?
					   TGPIO_OUTPUT_ARM_PERIODIC :
					   TGPIO_OUTPUT_FIRST_RISING;
	mmio_block->output_hw_first_art = 0;
	mmio_block->output_hw_piv = 0;

	if (mode != TGPIO_OUTPUT_HARDWARE) {
		ctrl = tgpio_read_ctl(mmio_block);
		tgpio_write_ctl(mmio_block, tgpio_ctl_with(ctrl, TGPIOCTL_EN));
	}
	return 0;
}

static struct tgpio_output_desired
tgpio_output_desired_get(struct tgpio_mmio_block *mmio_block)
{
	struct tgpio_output_desired desired;
	unsigned int seq;

	do {
		seq = read_seqbegin(&mmio_block->output_seqlock);
		desired = mmio_block->output_desired;
	} while (read_seqretry(&mmio_block->output_seqlock, seq));

	return desired;
}

static void tgpio_log_output_arm(struct tgpio_device *dev,
				 struct tgpio_mmio_block *mmio_block,
				 const struct tgpio_output_desired *desired,
				 const struct tgpio_output_timing *timing)
{
	struct tgpio_output_quantization quant;
	unsigned int channel;
	s64 start_adjust_ns;

	if (!READ_ONCE(activity_log))
		return;

	channel = tgpio_channel_for_mmio_block(dev, mmio_block->index,
					       PTP_PF_PEROUT);
	start_adjust_ns = timing->first_edge_ns - desired->first_edge_ns;

	if (desired->mode != TGPIO_OUTPUT_HARDWARE) {
		pr_info("activity=output_arm block=%u channel=%u mode=%s requested_period_ns=%llu high_time_ns=%llu low_time_ns=%llu requested_first_edge_ns=%lld armed_first_edge_ns=%lld start_adjust_ns=%lld prime_edge_ns=%lld\n",
			mmio_block->index, channel,
			tgpio_output_mode_name(desired->mode), desired->period_ns,
			desired->high_time_ns, desired->low_time_ns,
			desired->first_edge_ns, timing->first_edge_ns,
			start_adjust_ns, timing->prime_edge_ns);
		return;
	}

	quant = tgpio_output_quantization_get(dev, desired->period_ns,
					       desired->high_time_ns);

	if (quant.status < 0) {
		pr_info("activity=output_arm block=%u channel=%u mode=%s requested_period_ns=%llu high_time_ns=%llu low_time_ns=%llu quantization_status=%d requested_first_edge_ns=%lld armed_first_edge_ns=%lld start_adjust_ns=%lld prime_edge_ns=%lld\n",
			mmio_block->index, channel,
			tgpio_output_mode_name(desired->mode),
			desired->period_ns, desired->high_time_ns,
			desired->low_time_ns,
			quant.status, desired->first_edge_ns,
			timing->first_edge_ns, start_adjust_ns,
			timing->prime_edge_ns);
		return;
	}

	pr_info("activity=output_arm block=%u channel=%u mode=%s requested_period_ns=%llu high_time_ns=%llu low_time_ns=%llu clock_half_ns=%llu art_half_cycles=%llu actual_half_ns=%llu actual_period_ns=%llu half_error_ns=%lld period_error_ns=%lld period_split_error_ns=%lld requested_first_edge_ns=%lld armed_first_edge_ns=%lld start_adjust_ns=%lld prime_edge_ns=%lld\n",
		mmio_block->index, channel,
		tgpio_output_mode_name(desired->mode), desired->period_ns,
		desired->high_time_ns, desired->low_time_ns,
		quant.clock_half_period_ns, quant.art_half_period_cycles,
		quant.actual_half_period_ns, quant.actual_period_ns,
		quant.half_period_error_ns, quant.period_error_ns,
		quant.period_split_error_ns,
		desired->first_edge_ns, timing->first_edge_ns,
		start_adjust_ns, timing->prime_edge_ns);
}

static void tgpio_log_output_hw_periodic(struct tgpio_device *dev,
					 struct tgpio_mmio_block *mmio_block,
					 s64 first_edge_ns, u64 half_period_ns,
					 u64 first_art, u64 half_period_art)
{
	struct tgpio_ns_result actual_half;
	struct tgpio_s64_result period_error;
	unsigned int channel;
	s64 half_error_ns = 0;
	s64 period_error_ns = 0;
	u64 actual_period_ns = 0;

	if (!READ_ONCE(activity_log))
		return;

	actual_half = tgpio_clock_art_cycles_to_delta_ns(dev, half_period_art);
	if (actual_half.status >= 0 && actual_half.ns <= S64_MAX) {
		actual_period_ns = actual_half.ns * 2;
		half_error_ns = (s64)actual_half.ns - (s64)half_period_ns;
		period_error = tgpio_add_s64(half_error_ns, half_error_ns);
		if (period_error.status >= 0)
			period_error_ns = period_error.val;
	}

	channel = tgpio_channel_for_mmio_block(dev, mmio_block->index,
					       PTP_PF_PEROUT);
	pr_info("activity=output_hw_periodic block=%u channel=%u first_edge_ns=%lld first_art=%llu half_period_ns=%llu art_half_cycles=%llu actual_half_ns=%llu actual_period_ns=%llu half_error_ns=%lld period_error_ns=%lld\n",
		mmio_block->index, channel, first_edge_ns, first_art,
		half_period_ns, half_period_art,
		actual_half.status < 0 ? 0 : actual_half.ns, actual_period_ns,
		half_error_ns, period_error_ns);
}

static void tgpio_log_output_phase_rearm(struct tgpio_device *dev,
					 struct tgpio_mmio_block *mmio_block,
					 s64 phase_error_ns)
{
	unsigned int channel;

	if (!READ_ONCE(activity_log))
		return;

	channel = tgpio_channel_for_mmio_block(dev, mmio_block->index,
					       PTP_PF_PEROUT);
	pr_info("activity=output_phase_rearm block=%u channel=%u phase_error_ns=%lld\n",
		mmio_block->index, channel, phase_error_ns);
}

static void tgpio_log_output_phase_nudge(struct tgpio_device *dev,
					 struct tgpio_mmio_block *mmio_block,
					 s64 phase_error_ns, s64 aligned_ns)
{
	unsigned int channel;

	if (!READ_ONCE(activity_log))
		return;

	channel = tgpio_channel_for_mmio_block(dev, mmio_block->index,
					       PTP_PF_PEROUT);
	pr_info("activity=output_phase_nudge block=%u channel=%u phase_error_ns=%lld aligned_edge_ns=%lld\n",
		mmio_block->index, channel, phase_error_ns, aligned_ns);
}

static void tgpio_log_output_late_push(struct tgpio_device *dev,
				       struct tgpio_mmio_block *mmio_block,
				       u64 periods)
{
	unsigned int channel;

	if (!READ_ONCE(activity_log))
		return;

	channel = tgpio_channel_for_mmio_block(dev, mmio_block->index,
					       PTP_PF_PEROUT);
	pr_info("activity=output_late_push block=%u channel=%u pushed_periods=%llu\n",
		mmio_block->index, channel, periods);
}

static void tgpio_log_output_edge(struct tgpio_device *dev,
				  struct tgpio_mmio_block *mmio_block,
				  ktime_t edge_time, u32 edge_bits,
				  u64 interval_ns)
{
	unsigned int channel;

	if (!READ_ONCE(activity_log))
		return;

	channel = tgpio_channel_for_mmio_block(dev, mmio_block->index,
					       PTP_PF_PEROUT);
	pr_info("activity=output_edge block=%u channel=%u programmed_edge=%s edge_time_ns=%lld next_interval_ns=%llu\n",
		mmio_block->index, channel, tgpio_edge_bits_name(edge_bits),
		ktime_to_ns(edge_time), interval_ns);
}

static void tgpio_log_output_stop(struct tgpio_device *dev,
				  struct tgpio_mmio_block *mmio_block)
{
	unsigned int channel;

	if (!READ_ONCE(activity_log))
		return;

	channel = tgpio_channel_for_mmio_block(dev, mmio_block->index,
					       PTP_PF_PEROUT);
	pr_info("activity=output_stop block=%u channel=%u\n",
		mmio_block->index, channel);
}

/* Precise wake only: hand off to process context, touch no hardware. */
static enum hrtimer_restart tgpio_output_timer(struct hrtimer *timer)
{
	struct tgpio_mmio_block *mmio_block =
		container_of(timer, struct tgpio_mmio_block, output_timer);

	schedule_work(&mmio_block->output_work);
	return HRTIMER_NORESTART;
}

/* Re-arm the per-edge wake. Only output_work touches the timer. */
static void tgpio_output_wake_at(struct tgpio_mmio_block *mmio_block,
				 ktime_t when)
{
	mmio_block->output_wake_at = when;
	hrtimer_start(&mmio_block->output_timer, when, HRTIMER_MODE_ABS);
}

/*
 * output_work-only: full disable/prime/arm cycle against the current PHC
 * mapping, with the first edge kept on the requested period grid. This is
 * the only programming sequence the hardware honors, so both new configs and
 * phase corrections funnel through it.
 */
static void tgpio_reconcile_arm(struct tgpio_device *dev,
				struct tgpio_mmio_block *mmio_block,
				const struct tgpio_output_desired *desired)
{
	struct tgpio_output_timing timing = tgpio_prepare_output_timing(
		dev,
		tgpio_output_min_interval_ns(desired->high_time_ns,
					     desired->low_time_ns),
		desired->period_ns, desired->mode, desired->first_edge_ns);

	if (timing.status < 0 ||
	    tgpio_arm_output(dev, mmio_block, timing.first_edge_ns,
			     timing.prime_edge_ns, desired->high_time_ns,
			     desired->low_time_ns, desired->mode)) {
		hrtimer_cancel(&mmio_block->output_timer);
		tgpio_disable_output_hw(mmio_block);
		return;
	}
	mmio_block->output_applied_gen = desired->gen;
	mmio_block->output_applied_freq_gen = desired->freq_gen;
	tgpio_log_output_arm(dev, mmio_block, desired, &timing);
	tgpio_output_wake_at(mmio_block, timing.timer_start);
}

/*
 * output_work-only: apply a PHC frequency change to a running hardware
 * periodic block. PIV and COMPV hot-writes are verified safe on this
 * hardware: a PIV write latches for the following periods, and a COMPV
 * rewrite moves only the pending edge. Refresh the interval to the current
 * servo rate every update and nudge the pending edge back onto the grid
 * once it drifts past the tolerance, so no waveform restart is ever needed
 * in steady state.
 */
static void tgpio_hw_periodic_apply_freq(struct tgpio_device *dev,
					 struct tgpio_mmio_block *mmio_block,
					 const struct tgpio_output_desired *desired)
{
	struct tgpio_u64_result piv = tgpio_clock_delta_to_art_cycles(
		dev, mmio_block->output_high_time_ns);
	struct tgpio_hw_phase phase;
	struct tgpio_u64_result nudged_art;
	s64 aligned_ns;

	/* Checkpoint the level flop before changing PIV: the fold math needs
	 * the PIV that was live while the counted edges fired.
	 */
	tgpio_hw_flop_fold(mmio_block);

	/*
	 * Hot PIV writes are two 32-bit stores; only apply one when the high
	 * word is unchanged so a concurrent hardware reload cannot see a
	 * torn value. Mismatches fall back to the phase-error re-arm below.
	 */
	if (piv.status >= 0 && piv.val &&
	    piv.val != mmio_block->output_hw_piv &&
	    upper_32_bits(piv.val) ==
		    upper_32_bits(mmio_block->output_hw_piv)) {
		tgpio_write_piv(mmio_block, piv.val);
		mmio_block->output_hw_piv = piv.val;
	}

	phase = tgpio_hw_periodic_phase_get(dev, mmio_block,
					    desired->first_edge_ns,
					    desired->high_time_ns);
	if (phase.status < 0)
		return;

	if (tgpio_abs_s64(phase.error_ns) > desired->high_time_ns / 4) {
		/* Too close to the wrong half-grid slot: restart on the grid. */
		tgpio_log_output_phase_rearm(dev, mmio_block, phase.error_ns);
		tgpio_reconcile_arm(dev, mmio_block, desired);
		return;
	}

	if (phase.error_ns >= -TGPIO_OUTPUT_PHASE_NUDGE_NS &&
	    phase.error_ns <= TGPIO_OUTPUT_PHASE_NUDGE_NS)
		return;
	if (!phase.nudge_safe)
		return; /* edge imminent; retry on the next frequency update */

	aligned_ns = phase.pending_ns - phase.error_ns;
	nudged_art = tgpio_clock_ns_to_compare_art(dev, aligned_ns);
	if (nudged_art.status < 0)
		return;

	tgpio_write_compv(mmio_block, nudged_art.val);
	mmio_block->output_hw_ckpt_compv = nudged_art.val;
	mmio_block->output_next_edge = ns_to_ktime(aligned_ns);
	tgpio_log_output_phase_nudge(dev, mmio_block, phase.error_ns,
				     aligned_ns);
}

/*
 * output_work-only: before stopping a hardware periodic block, let the
 * pending falling edge fire when the tracked flop is high, leaving the flop
 * low. A fresh driver load assumes a low flop (the power-on state); this
 * keeps that assumption true across stop/start cycles and module reloads.
 * Bounded wait: gives up after ~3 seconds for very long periods.
 */
static void tgpio_hw_periodic_drain_low(struct tgpio_mmio_block *mmio_block)
{
	unsigned int i;

	if (mmio_block->output_phase != TGPIO_OUTPUT_HARDWARE_PERIODIC ||
	    !mmio_block->output_hw_piv)
		return;

	tgpio_hw_flop_fold(mmio_block);
	for (i = 0; mmio_block->output_hw_flop_high && i < 30; i++) {
		msleep(100);
		tgpio_hw_flop_fold(mmio_block);
	}
}

/*
 * output_work-only: external calibration told us the tracked level is
 * inverted. Fix the belief, and when the block free-runs, shift the pending
 * compare by half a period (one stretched half-cycle, no glitch) so rising
 * edges return to the requested grid.
 */
static void tgpio_hw_periodic_invert(struct tgpio_device *dev,
				     struct tgpio_mmio_block *mmio_block)
{
	struct tgpio_hw_phase phase;
	unsigned int i;
	u64 compv;

	mmio_block->output_hw_flop_high = !mmio_block->output_hw_flop_high;

	if (mmio_block->output_phase != TGPIO_OUTPUT_HARDWARE_PERIODIC ||
	    !mmio_block->output_hw_piv)
		return;

	/* Wait out an imminent edge so the COMPV rewrite cannot race it. */
	for (i = 0; i < 50; i++) {
		phase = tgpio_hw_periodic_phase_get(
			dev, mmio_block,
			tgpio_output_desired_get(mmio_block).first_edge_ns,
			mmio_block->output_high_time_ns);
		if (phase.status < 0 || phase.nudge_safe)
			break;
		msleep(20);
	}

	tgpio_hw_flop_fold(mmio_block);
	compv = tgpio_read_compv(mmio_block);
	if (compv > U64_MAX - mmio_block->output_hw_piv)
		return;
	compv += mmio_block->output_hw_piv;
	tgpio_write_compv(mmio_block, compv);
	mmio_block->output_hw_ckpt_compv = compv;
	pr_info("output polarity inverted block=%u flop_high=%d\n",
		mmio_block->index, mmio_block->output_hw_flop_high);
}

/* Sole writer of output hardware + the per-edge timer. */
static void tgpio_output_work(struct work_struct *work)
{
	struct tgpio_mmio_block *mmio_block =
		container_of(work, struct tgpio_mmio_block, output_work);
	struct tgpio_device *dev = mmio_block->owner;
	struct tgpio_output_desired desired =
		tgpio_output_desired_get(mmio_block);
	struct tgpio_u64_result interval;
	enum tgpio_logical_edge edge;
	u64 next_interval_ns;
	ktime_t next_edge;
	u32 edge_bits;

	if (desired.run == TGPIO_OUTPUT_STOPPED) {
		hrtimer_cancel(&mmio_block->output_timer);
		tgpio_hw_periodic_drain_low(mmio_block);
		tgpio_log_output_stop(dev, mmio_block);
		tgpio_disable_output_hw(mmio_block);
		return;
	}

	/* Calibration flips arrive out of band; apply the belief change
	 * before any re-arm so the arm picks the corrected slot.
	 */
	if ((desired.flip_gen - mmio_block->output_applied_flip_gen) & 1)
		tgpio_hw_periodic_invert(dev, mmio_block);
	mmio_block->output_applied_flip_gen = desired.flip_gen;

	/*
	 * New config: (re)arm to it. prepare and arm each read the PHC mapping
	 * separately, so a PHC step landing between them yields one period of mixed
	 * mapping -- the step's own gen bump (resync) then queues a reinit that
	 * corrects it. Eventually consistent, never wrong indefinitely.
	 */
	if (desired.gen != mmio_block->output_applied_gen) {
		tgpio_reconcile_arm(dev, mmio_block, &desired);
		return;
	}

	/*
	 * PHC frequency changed: hot-refresh PIV to the current servo rate
	 * and nudge the pending compare back onto the grid if it drifted.
	 * Both writes are verified safe on a running block.
	 */
	if (desired.freq_gen != mmio_block->output_applied_freq_gen) {
		mmio_block->output_applied_freq_gen = desired.freq_gen;
		if (mmio_block->output_phase ==
		    TGPIO_OUTPUT_HARDWARE_PERIODIC) {
			tgpio_hw_periodic_apply_freq(dev, mmio_block,
						     &desired);
			return;
		}
		if (mmio_block->output_phase == TGPIO_OUTPUT_ARM_PERIODIC &&
		    ktime_before(ktime_get_real(), mmio_block->output_wake_at))
			return;
	}

	/*
	 * Same generation: either the per-edge timer fired, or a PHC frequency
	 * refresh asked us to touch the hardware without re-priming. Programming an
	 * absolute compare early is harmless; the edge still waits for COMPV.
	 * Deliberately no wall-clock guard -- comparing now against output_wake_at
	 * would strand the line if CLOCK_REALTIME stepped backward between the timer
	 * firing and this work running.
	 */
	if (mmio_block->output_phase == TGPIO_OUTPUT_ARM_PERIODIC) {
		if (tgpio_arm_hardware_periodic(
			    dev, mmio_block,
			    ktime_to_ns(mmio_block->output_next_edge),
			    mmio_block->output_high_time_ns))
			tgpio_disable_output_hw(mmio_block);
		return; /* hardware free-runs; no further wake needed */
	}

	if (mmio_block->output_phase == TGPIO_OUTPUT_HARDWARE_PERIODIC)
		return;

	edge = mmio_block->output_next_edge_type;
	next_edge = mmio_block->output_next_edge;
	edge_bits = tgpio_output_edge_bits(edge);
	next_interval_ns = tgpio_output_interval_after_edge(mmio_block, edge);

	interval = tgpio_clock_delta_to_real_ns(dev, next_interval_ns);
	if (tgpio_program_output_edge(dev, mmio_block, next_edge, edge_bits) !=
		    TGPIO_OK ||
	    interval.status < 0 || interval.val == 0) {
		hrtimer_cancel(&mmio_block->output_timer);
		tgpio_disable_output_hw(mmio_block);
		return;
	}

	mmio_block->output_next_edge = ktime_add_ns(next_edge, next_interval_ns);
	mmio_block->output_next_edge_type = tgpio_output_opposite_edge(edge);
	mmio_block->output_phase = TGPIO_OUTPUT_TOGGLE;
	tgpio_log_output_edge(dev, mmio_block, next_edge, edge_bits,
			      interval.val);
	/*
	 * Advance the cadence from the previous wake, not from now, so workqueue
	 * latency doesn't erode the lead time period over period.
	 */
	tgpio_output_wake_at(mmio_block,
			     ktime_add_ns(mmio_block->output_wake_at,
					  interval.val));
}

static void tgpio_disable_output(struct tgpio_mmio_block *mmio_block)
{
	unsigned long flags;

	write_seqlock_irqsave(&mmio_block->output_seqlock, flags);
	mmio_block->output_desired = (struct tgpio_output_desired){
		.gen = mmio_block->output_desired.gen + 1,
		.freq_gen = mmio_block->output_desired.freq_gen,
		.flip_gen = mmio_block->output_desired.flip_gen,
		.run = TGPIO_OUTPUT_STOPPED,
	};
	write_sequnlock_irqrestore(&mmio_block->output_seqlock, flags);

	schedule_work(&mmio_block->output_work);
}

/* Publish a new running output config and schedule the work to reconcile it. */
static void tgpio_output_publish(struct tgpio_mmio_block *mmio_block,
				 enum tgpio_output_run run,
				 enum tgpio_output_mode mode,
				 u64 period_ns, u64 high_time_ns,
				 u64 low_time_ns,
				 s64 first_edge_ns)
{
	unsigned long flags;

	write_seqlock_irqsave(&mmio_block->output_seqlock, flags);
	mmio_block->output_desired = (struct tgpio_output_desired){
		.gen = mmio_block->output_desired.gen + 1,
		.freq_gen = mmio_block->output_desired.freq_gen,
		.flip_gen = mmio_block->output_desired.flip_gen,
		.run = run,
		.mode = mode,
		.period_ns = period_ns,
		.high_time_ns = high_time_ns,
		.low_time_ns = low_time_ns,
		.first_edge_ns = first_edge_ns,
	};
	write_sequnlock_irqrestore(&mmio_block->output_seqlock, flags);

	schedule_work(&mmio_block->output_work);
}

static int tgpio_config_output_values(struct tgpio_device *dev,
				      unsigned int channel, u64 period_ns,
				      u64 duty_ns, bool duty_valid,
				      s64 first_edge_ns, int on)
{
	struct tgpio_mmio_block *mmio_block;
	struct tgpio_output_times times;
	int block_index;
	enum tgpio_output_mode mode;

	block_index =
		tgpio_find_mmio_block_for_channel(dev, PTP_PF_PEROUT, channel);
	if (block_index < 0)
		return block_index;

	mmio_block = &dev->mmio_blocks[block_index];
	if (mmio_block->mode != TGPIO_MODE_OUTPUT || !mmio_block->regs)
		return -EOPNOTSUPP;

	if (!on) {
		tgpio_disable_output(mmio_block);
		return 0;
	}

	times = tgpio_output_times_get(period_ns, duty_ns, duty_valid);
	if (times.status < 0)
		return times.status;

	if (!timekeeping_clocksource_has_base(CSID_X86_ART)) {
		trace_tgpio_base_clock_lost(mmio_block->index);
		return -ENODEV;
	}

	if (hardware_periodic_output && tgpio_output_can_use_hardware(&times))
		mode = TGPIO_OUTPUT_HARDWARE;
	else
		mode = TGPIO_OUTPUT_SOFTWARE;
	if (mode == TGPIO_OUTPUT_HARDWARE &&
	    tgpio_clock_delta_to_art_cycles(dev, times.high_time_ns).status < 0)
		return -ENODEV;

	/* Validate the timing up front; output_work recomputes it on reconcile. */
	if (tgpio_prepare_output_timing(dev,
					tgpio_output_min_interval_ns(
						times.high_time_ns,
						times.low_time_ns),
					times.period_ns, mode, first_edge_ns)
		    .status < 0)
		return -ENODEV;

	tgpio_output_publish(mmio_block, TGPIO_OUTPUT_RUNNING, mode,
			     times.period_ns, times.high_time_ns,
			     times.low_time_ns, first_edge_ns);
	return 0;
}

static int tgpio_config_output(struct tgpio_device *dev,
			       const struct ptp_perout_request *perout, int on)
{
	struct tgpio_u64_result period;
	struct tgpio_u64_result duty = tgpio_ok_u64(0);
	struct tgpio_s64_result first_edge;
	unsigned int valid_flags = tgpio_supported_perout_flags();
	bool duty_valid = false;

	if (!on)
		return tgpio_config_output_values(dev, perout->index, 0, 0,
						  false, 0, 0);

	if (perout->flags & ~valid_flags)
		return -EOPNOTSUPP;

	period = tgpio_ptp_time_to_ns(&perout->period);
	if (period.status < 0)
		return period.status;

#ifdef PTP_PEROUT_DUTY_CYCLE
	if (perout->flags & PTP_PEROUT_DUTY_CYCLE) {
		duty = tgpio_ptp_time_to_ns(&perout->on);
		if (duty.status < 0)
			return duty.status;
		duty_valid = true;
	}
#endif

	first_edge = tgpio_ptp_time_to_s64_ns(&perout->start);
	if (first_edge.status < 0)
		return first_edge.status;

	return tgpio_config_output_values(dev, perout->index, period.val,
					  duty.val, duty_valid, first_edge.val,
					  on);
}

static int tgpio_start_persistent_input(struct tgpio_device *dev,
					unsigned int block_index)
{
	unsigned int channel = tgpio_persistent_input_channel(block_index);
	int ret;

	ret = tgpio_config_input(dev, channel, 0, 1);
	if (ret)
		return ret;

	pr_info("persisted input capture enabled block=%u channel=%u edge=%s\n",
		block_index, channel,
		tgpio_edge_bits_name(dev->mmio_blocks[block_index].input_edge_bits));
	return 0;
}

static int tgpio_start_persistent_output(struct tgpio_device *dev,
					 unsigned int block_index)
{
	struct tgpio_s64_result now;
	struct tgpio_s64_result first_edge;
	u64 period_ns = tgpio_persistent_output_period_ns(block_index);
	u64 duty_ns = tgpio_persistent_output_duty_ns(block_index);
	unsigned int channel = tgpio_persistent_output_channel(block_index);
	s64 first_edge_ns = 0;
	int ret;

	if (output_start_delay_ns) {
		if (output_start_delay_ns > S64_MAX)
			return -ERANGE;

		now = tgpio_clock_now_ns(dev);
		if (now.status < 0 || now.val < 0)
			return -ENODEV;

		first_edge = tgpio_add_s64(now.val,
					   (s64)output_start_delay_ns);
		if (first_edge.status < 0 || first_edge.val < 0)
			return -ERANGE;

		first_edge_ns = first_edge.val;
	}

	ret = tgpio_config_output_values(dev, channel, period_ns, duty_ns,
					 duty_ns != 0, first_edge_ns, 1);
	if (ret)
		return ret;

	pr_info("persisted output enabled block=%u channel=%u period_ns=%llu duty_ns=%llu start_delay_ns=%lu\n",
		block_index, channel, period_ns, duty_ns, output_start_delay_ns);
	return 0;
}

static int tgpio_start_persistent_operations(struct tgpio_device *dev)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		if (!tgpio_persistent_input_enabled(i))
			continue;

		ret = tgpio_start_persistent_input(dev, i);
		if (ret) {
			pr_err("failed to start persisted input%u: %d\n", i,
			       ret);
			return ret;
		}
	}

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		if (!tgpio_persistent_output_period_ns(i))
			continue;

		ret = tgpio_start_persistent_output(dev, i);
		if (ret) {
			pr_err("failed to start persisted output%u: %d\n", i,
			       ret);
			return ret;
		}
	}

	return 0;
}

static void tgpio_resync_outputs_after_phc_step(struct tgpio_device *dev)
{
	unsigned int i;

	if (clock_mode != TGPIO_CLOCK_PHC)
		return;

	/* Bump the generation of each running output so output_work re-arms
	 * against the new PHC mapping; the scheduled work runs promptly.
	 */
	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		struct tgpio_mmio_block *mmio_block = &dev->mmio_blocks[i];
		enum tgpio_output_run run;
		unsigned long flags;

		if (mmio_block->mode != TGPIO_MODE_OUTPUT)
			continue;

		write_seqlock_irqsave(&mmio_block->output_seqlock, flags);
		run = mmio_block->output_desired.run;
		if (run == TGPIO_OUTPUT_RUNNING)
			mmio_block->output_desired.gen++;
		write_sequnlock_irqrestore(&mmio_block->output_seqlock, flags);

		if (run == TGPIO_OUTPUT_RUNNING)
			schedule_work(&mmio_block->output_work);
	}
}

static void tgpio_update_outputs_after_phc_freq(struct tgpio_device *dev)
{
	unsigned int i;

	if (clock_mode != TGPIO_CLOCK_PHC)
		return;

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		struct tgpio_mmio_block *mmio_block = &dev->mmio_blocks[i];
		enum tgpio_output_run run;
		unsigned long flags;

		if (mmio_block->mode != TGPIO_MODE_OUTPUT)
			continue;

		write_seqlock_irqsave(&mmio_block->output_seqlock, flags);
		run = mmio_block->output_desired.run;
		if (run == TGPIO_OUTPUT_RUNNING)
			mmio_block->output_desired.freq_gen++;
		write_sequnlock_irqrestore(&mmio_block->output_seqlock, flags);

		if (run == TGPIO_OUTPUT_RUNNING)
			schedule_work(&mmio_block->output_work);
	}
}

static int tgpio_ptp_enable(struct ptp_clock_info *ptp,
			    struct ptp_clock_request *req, int on)
{
	struct tgpio_device *dev =
		container_of(ptp, struct tgpio_device, ptp_info);
	int ret;

	switch (req->type) {
	case PTP_CLK_REQ_EXTTS:
		ret = tgpio_config_input(dev, req->extts.index,
					 req->extts.flags, on);
		break;
	case PTP_CLK_REQ_PEROUT:
		ret = tgpio_config_output(dev, &req->perout, on);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static int tgpio_ptp_gettime64(struct ptp_clock_info *ptp,
			       struct timespec64 *ts)
{
	struct tgpio_device *dev =
		container_of(ptp, struct tgpio_device, ptp_info);
	struct tgpio_s64_result now;

	if (clock_mode != TGPIO_CLOCK_PHC) {
		ktime_get_real_ts64(ts);
		return 0;
	}

	now = tgpio_clock_phc_now_ns(dev);
	if (now.status < 0)
		return now.status;
	if (now.val < 0)
		return -ERANGE;

	*ts = ns_to_timespec64(now.val);
	return 0;
}

static int tgpio_ptp_settime64(struct ptp_clock_info *ptp,
			       const struct timespec64 *ts)
{
	struct tgpio_device *dev =
		container_of(ptp, struct tgpio_device, ptp_info);
	struct tgpio_u64_result art;
	struct tgpio_s64_result old;
	unsigned long flags;
	s64 ns;

	if (clock_mode != TGPIO_CLOCK_PHC)
		return -EOPNOTSUPP;

	if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= NSEC_PER_SEC)
		return -EINVAL;
	if (ts->tv_sec > div64_s64(S64_MAX, NSEC_PER_SEC))
		return -ERANGE;

	ns = timespec64_to_ns(ts);
	if (ns < 0)
		return -ERANGE;

	art = tgpio_get_current_art();
	if (art.status < 0)
		return -ENODEV;

	write_seqlock_irqsave(&dev->phc_seqlock, flags);
	old = tgpio_phc_art_to_ns(dev->phc, art.val);
	dev->phc = tgpio_phc_params_make(art.val, ns,
					 dev->phc.base_art_hz,
					 dev->phc.scaled_ppm);
	write_sequnlock_irqrestore(&dev->phc_seqlock, flags);

	trace_tgpio_phc_step(old.status < 0 ? 0 : old.val, ns);
	tgpio_resync_outputs_after_phc_step(dev);
	return 0;
}

static int tgpio_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct tgpio_device *dev =
		container_of(ptp, struct tgpio_device, ptp_info);
	struct tgpio_u64_result art;
	struct tgpio_s64_result now;
	struct tgpio_s64_result adjusted;
	unsigned long flags;

	if (clock_mode != TGPIO_CLOCK_PHC)
		return -EOPNOTSUPP;

	art = tgpio_get_current_art();
	if (art.status < 0)
		return -ENODEV;

	write_seqlock_irqsave(&dev->phc_seqlock, flags);
	now = tgpio_phc_art_to_ns(dev->phc, art.val);
	if (now.status < 0) {
		write_sequnlock_irqrestore(&dev->phc_seqlock, flags);
		return -ERANGE;
	}
	adjusted = tgpio_add_s64(now.val, delta);
	if (adjusted.status < 0 || adjusted.val < 0) {
		write_sequnlock_irqrestore(&dev->phc_seqlock, flags);
		return -ERANGE;
	}
	dev->phc = tgpio_phc_params_make(art.val, adjusted.val,
					 dev->phc.base_art_hz,
					 dev->phc.scaled_ppm);
	write_sequnlock_irqrestore(&dev->phc_seqlock, flags);

	trace_tgpio_phc_step(now.val, adjusted.val);
	tgpio_resync_outputs_after_phc_step(dev);
	return 0;
}

static int tgpio_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct tgpio_device *dev =
		container_of(ptp, struct tgpio_device, ptp_info);
	struct tgpio_u64_result art;
	struct tgpio_s64_result now;
	unsigned long flags;
	s64 ppb;

	if (clock_mode != TGPIO_CLOCK_PHC)
		return -EOPNOTSUPP;

	ppb = tgpio_scaled_ppm_to_ppb(scaled_ppm);
	if (ppb > TGPIO_PHC_MAX_ADJ_PPB || ppb < -TGPIO_PHC_MAX_ADJ_PPB)
		return -ERANGE;

	art = tgpio_get_current_art();
	if (art.status < 0)
		return -ENODEV;

	write_seqlock_irqsave(&dev->phc_seqlock, flags);
	now = tgpio_phc_art_to_ns(dev->phc, art.val);
	if (now.status < 0) {
		write_sequnlock_irqrestore(&dev->phc_seqlock, flags);
		return -ERANGE;
	}
	dev->phc = tgpio_phc_params_make(art.val, now.val,
					 dev->phc.base_art_hz, scaled_ppm);
	write_sequnlock_irqrestore(&dev->phc_seqlock, flags);

	if (hardware_periodic_output)
		tgpio_update_outputs_after_phc_freq(dev);
	return 0;
}

static int tgpio_ptp_verify(struct ptp_clock_info *ptp, unsigned int pin,
			    enum ptp_pin_function func, unsigned int chan)
{
	struct tgpio_device *dev =
		container_of(ptp, struct tgpio_device, ptp_info);

	if (pin >= dev->n_ptp_pins)
		return -EINVAL;

	if (func == PTP_PF_NONE)
		return 0;

	if (chan >= dev->n_ptp_pins)
		return -EINVAL;

	switch (func) {
	case PTP_PF_EXTTS:
		if (tgpio_mmio_block_supports_func(dev, pin, PTP_PF_EXTTS) ==
		    TGPIO_UNSUPPORTED)
			return -EOPNOTSUPP;
		return 0;
	case PTP_PF_PEROUT:
		if (tgpio_mmio_block_supports_func(dev, pin, PTP_PF_PEROUT) ==
		    TGPIO_UNSUPPORTED)
			return -EOPNOTSUPP;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static void tgpio_disable_inputs(struct tgpio_device *dev)
{
	unsigned int i;

	cancel_delayed_work_sync(&dev->poll_work);

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		struct tgpio_mmio_block *mmio_block = &dev->mmio_blocks[i];
		u32 ctrl;

		if (mmio_block->mode != TGPIO_MODE_INPUT || !mmio_block->regs)
			continue;

		ctrl = tgpio_read_ctl(mmio_block);
		ctrl &= ~TGPIOCTL_EN;
		tgpio_write_ctl(mmio_block, ctrl);
		atomic_set(&mmio_block->input_desired, 0);
	}
}

static void tgpio_disable_outputs(struct tgpio_device *dev)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		struct tgpio_mmio_block *mmio_block = &dev->mmio_blocks[i];
		unsigned long flags;

		if (mmio_block->mode != TGPIO_MODE_OUTPUT)
			continue;

		write_seqlock_irqsave(&mmio_block->output_seqlock, flags);
		mmio_block->output_desired = (struct tgpio_output_desired){
			.gen = mmio_block->output_desired.gen + 1,
			.freq_gen = mmio_block->output_desired.freq_gen,
			.flip_gen = mmio_block->output_desired.flip_gen,
			.run = TGPIO_OUTPUT_STOPPED,
		};
		write_sequnlock_irqrestore(&mmio_block->output_seqlock, flags);

		/*
		 * A work that snapshotted RUNNING before the STOPPED publish can
		 * re-arm the timer after the first cancel. STOPPED is now visible,
		 * so any later work run won't re-arm; a second round drains the
		 * timer->work->timer chain to quiescence before unmap/free.
		 */
		hrtimer_cancel(&mmio_block->output_timer);
		cancel_work_sync(&mmio_block->output_work);
		hrtimer_cancel(&mmio_block->output_timer);
		cancel_work_sync(&mmio_block->output_work);
		if (mmio_block->regs) {
			/*
			 * The work is quiesced, so its private output state
			 * is safe to touch. Leave the level flop low so the
			 * next driver load can trust its power-on assumption.
			 */
			tgpio_hw_periodic_drain_low(mmio_block);
			tgpio_disable_output_hw(mmio_block);
		}
	}
}

static void tgpio_disable_mmio_blocks(struct tgpio_device *dev)
{
	tgpio_disable_inputs(dev);
	tgpio_disable_outputs(dev);
}

static void tgpio_unmap_mmio_blocks(struct tgpio_device *dev)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		struct tgpio_mmio_block *mmio_block = &dev->mmio_blocks[i];

		if (mmio_block->regs) {
			iounmap(mmio_block->regs);
			mmio_block->regs = NULL;
		}
		if (mmio_block->mem_region) {
			release_mem_region(mmio_block->mmio_phys, mmio_size);
			mmio_block->mem_region = NULL;
		}
	}
}

static int tgpio_map_mmio_block(struct tgpio_device *dev, unsigned int index)
{
	struct tgpio_mmio_block *mmio_block = &dev->mmio_blocks[index];
	int ret;

	ret = tgpio_check_addr(mmio_block->mmio_phys);
	if (ret)
		return ret;

	mmio_block->mem_region = request_mem_region(mmio_block->mmio_phys,
						    mmio_size, KBUILD_MODNAME);
	if (!mmio_block->mem_region) {
		pr_err("MMIO range %#lx-%#lx is busy\n", mmio_block->mmio_phys,
		       mmio_block->mmio_phys + mmio_size - 1);
		return -EBUSY;
	}

	mmio_block->regs = ioremap(mmio_block->mmio_phys, mmio_size);
	if (!mmio_block->regs) {
		release_mem_region(mmio_block->mmio_phys, mmio_size);
		mmio_block->mem_region = NULL;
		return -ENOMEM;
	}

	dev->n_ptp_pins = max(dev->n_ptp_pins, index + 1);
	pr_info("block %u %s at %#lx-%#lx\n", index,
		tgpio_mode_name(mmio_block->mode), mmio_block->mmio_phys,
		mmio_block->mmio_phys + mmio_size - 1);
	return 0;
}

static int tgpio_configure_mmio_blocks(struct tgpio_device *dev)
{
	const char *modes[TGPIO_MAX_BLOCKS] = { mode0_param, mode1_param };
	const char *edges[TGPIO_MAX_BLOCKS] = { edge0_param, edge1_param };
	unsigned long addrs[TGPIO_MAX_BLOCKS] = { addr0, addr1 };
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		struct tgpio_mmio_block *mmio_block = &dev->mmio_blocks[i];

		mmio_block->owner = dev;
		mmio_block->index = i;
		mmio_block->mmio_phys = addrs[i];
		seqlock_init(&mmio_block->output_seqlock);
		INIT_WORK(&mmio_block->output_work, tgpio_output_work);
		hrtimer_setup(&mmio_block->output_timer, tgpio_output_timer,
			      CLOCK_REALTIME, HRTIMER_MODE_ABS);

		struct tgpio_mode_result mode = tgpio_parse_mode(modes[i]);
		struct tgpio_edge_result edge = tgpio_parse_edge(edges[i]);

		if (mode.status < 0) {
			pr_err("invalid mode%u=%s; use input, output, or off\n",
			       i, modes[i]);
			return mode.status;
		}
		mmio_block->mode = mode.mode;

		if (edge.status < 0) {
			pr_err("invalid edge%u=%s; use rising, falling, or both\n",
			       i, edges[i]);
			return edge.status;
		}
		mmio_block->input_edge_bits = edge.edge_bits;
	}

	if (!use_second)
		dev->mmio_blocks[1].mode = TGPIO_MODE_OFF;

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		struct tgpio_mmio_block *mmio_block = &dev->mmio_blocks[i];

		switch (mmio_block->mode) {
		case TGPIO_MODE_INPUT:
		case TGPIO_MODE_OUTPUT:
			ret = tgpio_map_mmio_block(dev, i);
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

static enum tgpio_requirement
tgpio_needs_art_frequency(struct tgpio_device *dev)
{
	unsigned int i;

	if (!hardware_timestamps)
		return TGPIO_NOT_REQUIRED;

	if (timestamp_mode != TGPIO_TIMESTAMP_ART)
		return TGPIO_NOT_REQUIRED;

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		if (dev->mmio_blocks[i].mode == TGPIO_MODE_INPUT)
			return TGPIO_REQUIRED;
	}

	return TGPIO_NOT_REQUIRED;
}

static enum tgpio_requirement
tgpio_needs_art_base_clock(struct tgpio_device *dev)
{
	unsigned int i;

	if (!hardware_timestamps)
		return TGPIO_NOT_REQUIRED;

	if (timestamp_mode != TGPIO_TIMESTAMP_REALTIME)
		return TGPIO_NOT_REQUIRED;

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		if (dev->mmio_blocks[i].mode == TGPIO_MODE_INPUT)
			return TGPIO_REQUIRED;
	}

	return TGPIO_NOT_REQUIRED;
}

static int tgpio_validate_persistent_operations(struct tgpio_device *dev)
{
	unsigned int i;
	unsigned int j;

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		struct tgpio_mmio_block *mmio_block = &dev->mmio_blocks[i];
		bool input_enabled = tgpio_persistent_input_enabled(i);
		u64 output_period_ns = tgpio_persistent_output_period_ns(i);
		u64 output_duty_ns = tgpio_persistent_output_duty_ns(i);
		struct tgpio_output_times output_times;

		if (input_enabled && output_period_ns) {
			pr_err("block %u cannot persist both input capture and output generation\n",
			       i);
			return -EINVAL;
		}

		if (output_period_ns > S64_MAX) {
			pr_err("output%u_period_ns=%llu is too large\n", i,
			       output_period_ns);
			return -ERANGE;
		}

		if (output_duty_ns && !output_period_ns) {
			pr_err("output%u_duty_ns requires output%u_period_ns\n", i,
			       i);
			return -EINVAL;
		}

		output_times = tgpio_output_times_get(output_period_ns,
						     output_duty_ns,
						     output_duty_ns != 0);
		if (output_period_ns && output_times.status < 0) {
			pr_err("output%u duty configuration is invalid: period_ns=%llu duty_ns=%llu\n",
			       i, output_period_ns, output_duty_ns);
			return output_times.status;
		}

		if (input_enabled && mmio_block->mode != TGPIO_MODE_INPUT) {
			pr_err("input%u_enable requires mode%u=input\n", i, i);
			return -EINVAL;
		}

		if (output_period_ns &&
		    mmio_block->mode != TGPIO_MODE_OUTPUT) {
			pr_err("output%u_period_ns requires mode%u=output\n",
			       i, i);
			return -EINVAL;
		}

		if (input_enabled &&
		    tgpio_persistent_input_channel(i) >= dev->n_ptp_pins) {
			pr_err("input%u_channel=%u exceeds available PTP channels\n",
			       i, tgpio_persistent_input_channel(i));
			return -EINVAL;
		}

		if (output_period_ns &&
		    tgpio_persistent_output_channel(i) >= dev->n_ptp_pins) {
			pr_err("output%u_channel=%u exceeds available PTP channels\n",
			       i, tgpio_persistent_output_channel(i));
			return -EINVAL;
		}
	}

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		for (j = i + 1; j < ARRAY_SIZE(dev->mmio_blocks); j++) {
			if (tgpio_persistent_input_enabled(i) &&
			    tgpio_persistent_input_enabled(j) &&
			    tgpio_persistent_input_channel(i) ==
				    tgpio_persistent_input_channel(j)) {
				pr_err("input%u_channel and input%u_channel both use PTP channel %u\n",
				       i, j,
				       tgpio_persistent_input_channel(i));
				return -EINVAL;
			}

			if (tgpio_persistent_output_period_ns(i) &&
			    tgpio_persistent_output_period_ns(j) &&
			    tgpio_persistent_output_channel(i) ==
				    tgpio_persistent_output_channel(j)) {
				pr_err("output%u_channel and output%u_channel both use PTP channel %u\n",
				       i, j,
				       tgpio_persistent_output_channel(i));
				return -EINVAL;
			}
		}
	}

	return 0;
}

static void tgpio_setup_pin_descs(struct tgpio_device *dev)
{
	unsigned int i;

	for (i = 0; i < dev->n_ptp_pins; i++) {
		snprintf(dev->pin_config[i].name,
			 sizeof(dev->pin_config[i].name), "tgpio%u-%s", i,
			 tgpio_mode_name(dev->mmio_blocks[i].mode));
		dev->pin_config[i].index = i;
		dev->pin_config[i].func = PTP_PF_NONE;
		dev->pin_config[i].chan = i;

		if (tgpio_persistent_input_enabled(i)) {
			dev->pin_config[i].func = PTP_PF_EXTTS;
			dev->pin_config[i].chan =
				tgpio_persistent_input_channel(i);
		} else if (tgpio_persistent_output_period_ns(i)) {
			dev->pin_config[i].func = PTP_PF_PEROUT;
			dev->pin_config[i].chan =
				tgpio_persistent_output_channel(i);
		}
	}
}

static int tgpio_register_ptp_clock(struct tgpio_device *dev)
{
	int ret;

	if (!dev->n_ptp_pins)
		return 0;

	tgpio_setup_pin_descs(dev);

	dev->ptp_info.owner = THIS_MODULE;
	snprintf(dev->ptp_info.name, sizeof(dev->ptp_info.name), "Intel TGPIO");
	dev->ptp_info.max_adj =
		clock_mode == TGPIO_CLOCK_PHC ? TGPIO_PHC_MAX_ADJ_PPB : 0;
	dev->ptp_info.n_pins = dev->n_ptp_pins;
	dev->ptp_info.n_ext_ts = dev->n_ptp_pins;
	dev->ptp_info.n_per_out = dev->n_ptp_pins;
	dev->ptp_info.supported_perout_flags = tgpio_supported_perout_flags();
	dev->ptp_info.pin_config = dev->pin_config;
	dev->ptp_info.adjfine = tgpio_ptp_adjfine;
	dev->ptp_info.adjtime = tgpio_ptp_adjtime;
	dev->ptp_info.gettime64 = tgpio_ptp_gettime64;
	dev->ptp_info.settime64 = tgpio_ptp_settime64;
	dev->ptp_info.enable = tgpio_ptp_enable;
	dev->ptp_info.verify = tgpio_ptp_verify;

	dev->ptp_clock = ptp_clock_register(&dev->ptp_info, NULL);
	if (IS_ERR(dev->ptp_clock)) {
		ret = PTR_ERR(dev->ptp_clock);
		dev->ptp_clock = NULL;
		return ret;
	}

	pr_info("registered PTP clock with %u pin slot(s)\n", dev->n_ptp_pins);
	return 0;
}

/* Observability: debugfs status dump and sysfs counters; tracepoints push. */
static int tgpio_status_show(struct seq_file *m, void *v)
{
	struct tgpio_device *dev = m->private;
	struct tgpio_phc_params phc = tgpio_phc_params_get(dev);
	struct system_time_snapshot snapshot;
	u64 snapshot_art;
	unsigned int i;

	tgpio_get_realtime_snapshot(&snapshot);

	seq_printf(m, "clock_mode: %s\n", tgpio_clock_mode_name(clock_mode));
	seq_printf(m, "art_frequency: %lu Hz\n", art_frequency);
	seq_printf(m, "activity_log: %s\n",
		   READ_ONCE(activity_log) ? "on" : "off");
	seq_printf(m, "art_base_clock: %s\n",
		   timekeeping_clocksource_has_base(CSID_X86_ART) ? "present" :
								    "absent");
	seq_printf(m, "art_snapshot: %s\n",
		   tgpio_snapshot_art_cycles(&snapshot, &snapshot_art) ?
			   "present" :
			   "absent");
	seq_printf(m,
		   "phc: anchor_art=%llu anchor_ns=%lld base_art_hz=%llu scaled_ppm=%ld\n",
		   phc.anchor_art, phc.anchor_ns, phc.base_art_hz,
		   phc.scaled_ppm);

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		struct tgpio_mmio_block *mmio_block = &dev->mmio_blocks[i];

		seq_printf(m, "block%u: %s", i,
			   tgpio_mode_name(mmio_block->mode));
		if (mmio_block->mode == TGPIO_MODE_INPUT) {
			seq_printf(m, " capture=%s events=%llu",
				   tgpio_input_desired_state(atomic_read(
					   &mmio_block->input_desired)) ==
						   TGPIO_CAPTURE_ON ?
					   "on" :
					   "off",
				   mmio_block->last_event_count);
		} else if (mmio_block->mode == TGPIO_MODE_OUTPUT) {
			struct tgpio_output_desired desired =
				tgpio_output_desired_get(mmio_block);
			u64 output_high_time_ns =
				desired.run == TGPIO_OUTPUT_RUNNING ?
					READ_ONCE(mmio_block->output_high_time_ns) :
					desired.high_time_ns;
			u64 output_low_time_ns =
				desired.run == TGPIO_OUTPUT_RUNNING ?
					READ_ONCE(mmio_block->output_low_time_ns) :
					desired.low_time_ns;

			seq_printf(m,
				   " %s mode=%s period=%lluns high_time=%lluns low_time=%lluns",
				   desired.run == TGPIO_OUTPUT_RUNNING ?
					   "running" :
					   "stopped",
				   tgpio_output_mode_name(desired.mode),
				   desired.period_ns, output_high_time_ns,
				   output_low_time_ns);
			if (desired.run == TGPIO_OUTPUT_RUNNING &&
			    desired.mode == TGPIO_OUTPUT_HARDWARE &&
			    output_high_time_ns == output_low_time_ns &&
			    output_high_time_ns) {
				struct tgpio_output_quantization quant =
					tgpio_output_quantization_get(
						dev, desired.period_ns,
						output_high_time_ns);

				if (quant.status >= 0)
					seq_printf(m,
						   " art_half_cycles=%llu actual_period=%lluns period_error=%lldns",
						   quant.art_half_period_cycles,
						   quant.actual_period_ns,
						   quant.period_error_ns);
				else
					seq_printf(m,
						   " quantization_status=%d",
						   quant.status);

				if (READ_ONCE(mmio_block->output_hw_piv)) {
					struct tgpio_hw_phase phase =
						tgpio_hw_periodic_phase_get(
							dev, mmio_block,
							desired.first_edge_ns,
							output_high_time_ns);

					if (phase.status >= 0)
						seq_printf(m,
							   " armed_piv=%llu phase_error=%lldns",
							   READ_ONCE(mmio_block->output_hw_piv),
							   phase.error_ns);
					seq_printf(m, " tracked_level=%s",
						   READ_ONCE(mmio_block->output_hw_flop_high) ?
							   "high" :
							   "low");
				}
			}
		}
		seq_putc(m, '\n');
	}

	seq_printf(m, "counters: events=%lld fallbacks=%lld\n",
		   atomic64_read(&dev->stats.events),
		   atomic64_read(&dev->stats.fallbacks));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(tgpio_status);

static struct dentry *tgpio_debugfs_dir;

/*
 * Write 1 to flip the tracked output level and shift a running waveform by
 * half a period. This is the one-shot calibration hook for the write-only
 * hardware level flop: the driver assumes it is low at load, and an external
 * observer (scope, logic analyzer, timestamper) corrects it here if the
 * assumption was wrong.
 */
static ssize_t tgpio_output_invert_write(struct file *file,
					 const char __user *ubuf, size_t count,
					 loff_t *ppos)
{
	struct tgpio_mmio_block *mmio_block = file->private_data;
	unsigned long flags;
	bool value;
	int ret;

	ret = kstrtobool_from_user(ubuf, count, &value);
	if (ret)
		return ret;
	if (!value)
		return count;

	write_seqlock_irqsave(&mmio_block->output_seqlock, flags);
	mmio_block->output_desired.flip_gen++;
	write_sequnlock_irqrestore(&mmio_block->output_seqlock, flags);
	schedule_work(&mmio_block->output_work);
	return count;
}

static const struct file_operations tgpio_output_invert_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = tgpio_output_invert_write,
};

static void tgpio_debugfs_init(struct tgpio_device *dev)
{
	unsigned int i;

	tgpio_debugfs_dir = debugfs_create_dir("tgpio", NULL);
	debugfs_create_file("status", 0444, tgpio_debugfs_dir, dev,
			    &tgpio_status_fops);

	for (i = 0; i < ARRAY_SIZE(dev->mmio_blocks); i++) {
		struct tgpio_mmio_block *mmio_block = &dev->mmio_blocks[i];
		char name[24];

		if (mmio_block->mode != TGPIO_MODE_OUTPUT)
			continue;
		snprintf(name, sizeof(name), "output%u_invert", i);
		debugfs_create_file(name, 0200, tgpio_debugfs_dir, mmio_block,
				    &tgpio_output_invert_fops);
	}
}

static void tgpio_debugfs_exit(void)
{
	debugfs_remove_recursive(tgpio_debugfs_dir);
	tgpio_debugfs_dir = NULL;
}

static ssize_t events_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return sysfs_emit(buf, "%lld\n", atomic64_read(&tgpio->stats.events));
}
static struct kobj_attribute tgpio_events_attr = __ATTR_RO(events);

static ssize_t fallbacks_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf, "%lld\n",
			  atomic64_read(&tgpio->stats.fallbacks));
}
static struct kobj_attribute tgpio_fallbacks_attr = __ATTR_RO(fallbacks);

static struct attribute *tgpio_sysfs_attrs[] = {
	&tgpio_events_attr.attr,
	&tgpio_fallbacks_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(tgpio_sysfs);

static struct kobject *tgpio_sysfs_kobj;

static int tgpio_sysfs_init(void)
{
	tgpio_sysfs_kobj = kobject_create_and_add("tgpio", kernel_kobj);
	if (!tgpio_sysfs_kobj)
		return -ENOMEM;

	return sysfs_create_groups(tgpio_sysfs_kobj, tgpio_sysfs_groups);
}

static void tgpio_sysfs_exit(void)
{
	kobject_put(tgpio_sysfs_kobj);
	tgpio_sysfs_kobj = NULL;
}

static void tgpio_observability_init(struct tgpio_device *dev)
{
	tgpio_debugfs_init(dev);
	if (tgpio_sysfs_init())
		pr_warn("failed to create sysfs counters\n");
}

static void tgpio_observability_exit(void)
{
	tgpio_sysfs_exit();
	tgpio_debugfs_exit();
}

static int __init tgpio_input_init(void)
{
	struct tgpio_clock_mode_result clock;
	struct tgpio_timestamp_mode_result timestamp;
	struct tgpio_output_polarity_result polarity;
	int ret;

	if (mmio_size < TGPIO_MIN_MMIO_SIZE) {
		pr_err("mmio_size must be at least %#x bytes\n",
		       TGPIO_MIN_MMIO_SIZE);
		return -EINVAL;
	}

	tgpio = kzalloc(sizeof(*tgpio), GFP_KERNEL);
	if (!tgpio)
		return -ENOMEM;

	seqlock_init(&tgpio->phc_seqlock);
	INIT_DELAYED_WORK(&tgpio->poll_work, tgpio_poll_work);

	clock = tgpio_parse_clock_mode(clock_mode_param);
	if (clock.status < 0) {
		pr_err("invalid clock_mode=%s; use realtime or phc\n",
		       clock_mode_param);
		ret = clock.status;
		goto err_cleanup;
	}
	clock_mode = clock.mode;

	timestamp = tgpio_parse_timestamp_mode(timestamp_mode_param);
	if (timestamp.status < 0) {
		pr_err("invalid timestamp_mode=%s; use realtime or art\n",
		       timestamp_mode_param);
		ret = timestamp.status;
		goto err_cleanup;
	}
	timestamp_mode = timestamp.mode;

	polarity = tgpio_parse_output_polarity(output_polarity_param);
	if (polarity.status < 0) {
		pr_err("invalid output_polarity=%s; use normal or inverted\n",
		       output_polarity_param);
		ret = polarity.status;
		goto err_cleanup;
	}
	output_polarity = polarity.polarity;

	ret = tgpio_configure_mmio_blocks(tgpio);
	if (ret)
		goto err_cleanup;

	ret = tgpio_validate_persistent_operations(tgpio);
	if (ret)
		goto err_cleanup;

	if (clock_mode == TGPIO_CLOCK_PHC) {
		if (!timekeeping_clocksource_has_base(CSID_X86_ART)) {
			pr_err("clock_mode=phc requires a timekeeper clocksource based on ART\n");
			ret = -ENODEV;
			goto err_cleanup;
		}

		ret = tgpio_phc_init_clock(tgpio);
		if (ret)
			goto err_cleanup;

		tgpio_probe_art_parameters_for_status();
		pr_info("PTP clock uses adjustable ART-backed PHC mode\n");
	} else if (tgpio_needs_art_frequency(tgpio) == TGPIO_REQUIRED) {
		ret = tgpio_resolve_art_frequency();
		if (ret)
			goto err_cleanup;
	} else if (tgpio_needs_art_base_clock(tgpio) == TGPIO_REQUIRED) {
		if (!timekeeping_clocksource_has_base(CSID_X86_ART)) {
			pr_err("timestamp_mode=realtime requires a timekeeper clocksource based on ART; use timestamp_mode=art or hardware_timestamps=0\n");
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

	tgpio_observability_init(tgpio);

	ret = tgpio_start_persistent_operations(tgpio);
	if (ret)
		goto err_cleanup;

	pr_info("loaded with mode0=%s mode1=%s clock_mode=%s timestamp_mode=%s output_polarity=%s hardware_periodic_output=%c activity_log=%c\n",
		tgpio_mode_name(tgpio->mmio_blocks[0].mode),
		tgpio_mode_name(tgpio->mmio_blocks[1].mode),
		tgpio_clock_mode_name(clock_mode),
		tgpio_timestamp_mode_name(timestamp_mode),
		tgpio_output_polarity_name(output_polarity),
		hardware_periodic_output ? 'Y' : 'N',
		activity_log ? 'Y' : 'N');
	return 0;

err_cleanup:
	tgpio_observability_exit();
	if (tgpio->ptp_clock)
		ptp_clock_unregister(tgpio->ptp_clock);
	tgpio_disable_mmio_blocks(tgpio);
	tgpio_unmap_mmio_blocks(tgpio);
	kfree(tgpio);
	tgpio = NULL;
	return ret;
}

static void __exit tgpio_input_exit(void)
{
	if (!tgpio)
		return;

	tgpio_observability_exit();

	/* Unregister first: no new PTP enable callbacks can republish or reschedule
	 * once this returns, so the cancels below are final.
	 */
	if (tgpio->ptp_clock)
		ptp_clock_unregister(tgpio->ptp_clock);

	tgpio_disable_mmio_blocks(tgpio);

	tgpio_unmap_mmio_blocks(tgpio);

	kfree(tgpio);
	tgpio = NULL;
}

module_init(tgpio_input_init);
module_exit(tgpio_input_exit);

MODULE_AUTHOR("Ahmad Byagowi");
MODULE_DESCRIPTION("Intel TGPIO per-block PTP input/output add-on driver");
MODULE_LICENSE("Dual BSD/GPL");

// Licensed under the TGPIO Non-Commercial License (see LICENSE).
// Commercial use requires the prior written permission of Ahmad Byagowi.

/*
 * Intel Time-Aware GPIO -- Windows KMDF driver, ART time base.
 * Copyright (c) 2026 Ahmad Byagowi
 *
 * Windows has no PTP hardware clock abstraction and no kernel ART
 * clocksource, so this driver derives ART from the invariant TSC:
 *
 *     TSC = ART * numerator / denominator + IA32_TSC_ADJUST
 *
 * with the ratio and the nominal crystal frequency from CPUID leaf 0x15
 * (the same leaf the Linux driver uses). All wall-clock correlation is
 * left to user mode through the crosststamp IOCTL.
 */

#include "tgpio.h"

#define TGPIO_CPUID_ART_LEAF 0x15
#define MSR_IA32_TSC_ADJUST  0x3b

static unsigned __int64 tgpio_muldiv(unsigned __int64 value,
				     unsigned __int64 mul,
				     unsigned __int64 div)
{
	unsigned __int64 hi;
	unsigned __int64 lo = _umul128(value, mul, &hi);
	unsigned __int64 rem;

	if (hi >= div) /* quotient would overflow 64 bits */
		return MAXULONG64;

	return _udiv128(hi, lo, div, &rem);
}

NTSTATUS tgpio_art_detect(struct tgpio_win_device *dev)
{
	int regs[4];

	__cpuid(regs, 0);
	if ((ULONG)regs[0] < TGPIO_CPUID_ART_LEAF)
		return STATUS_NOT_SUPPORTED;

	__cpuidex(regs, TGPIO_CPUID_ART_LEAF, 0);
	if (!regs[0] || !regs[1])
		return STATUS_NOT_SUPPORTED;

	dev->tsc_art_denominator = (ULONG)regs[0]; /* EAX */
	dev->tsc_art_numerator = (ULONG)regs[1]; /* EBX */
	dev->art_frequency_hz = (ULONG)regs[2]; /* ECX, may be 0 */

	/*
	 * IA32_TSC_ADJUST offsets the TSC from the ART-derived value. Read
	 * it once at load; firmware writes it at boot and nothing on a
	 * quiet test box moves it afterwards. Guarded: pre-Haswell parts
	 * without the MSR would #GP.
	 */
	__cpuidex(regs, 7, 0);
	if (regs[1] & (1 << 1)) /* EBX bit 1: TSC_ADJUST supported */
		dev->tsc_adjust = __readmsr(MSR_IA32_TSC_ADJUST);
	else
		dev->tsc_adjust = 0;

	/*
	 * Client SKUs often leave ECX zero (crystal not enumerated); Linux
	 * fills it from kernel frequency tables. Here the caller may
	 * override from the ArtFrequencyHz registry parameter, so a zero
	 * frequency is not fatal as long as the ratio is valid.
	 */
	return STATUS_SUCCESS;
}

unsigned __int64 tgpio_art_from_tsc(const struct tgpio_win_device *dev,
				    unsigned __int64 tsc)
{
	unsigned __int64 base = tsc - dev->tsc_adjust;

	return tgpio_muldiv(base, dev->tsc_art_denominator,
			    dev->tsc_art_numerator);
}

unsigned __int64 tgpio_art_now(const struct tgpio_win_device *dev)
{
	return tgpio_art_from_tsc(dev, __rdtsc());
}

unsigned __int64 tgpio_art_ns_to_cycles(const struct tgpio_win_device *dev,
					unsigned __int64 ns)
{
	return tgpio_muldiv(ns, dev->art_frequency_hz, 1000000000ull);
}

/*
 * Bracket a precise system time read between two TSC reads. User mode uses
 * the midpoint TSC as the instant systime_100ns was taken; the bracket
 * width bounds the pairing error (typically well under 100 ns).
 */
void tgpio_art_crosststamp(const struct tgpio_win_device *dev,
			   struct tgpio_win_crosststamp *xt)
{
	LARGE_INTEGER systime;
	KIRQL irql;

	KeRaiseIrql(DISPATCH_LEVEL, &irql);
	xt->tsc_before = __rdtsc();
	KeQuerySystemTimePrecise(&systime);
	xt->tsc_after = __rdtsc();
	KeLowerIrql(irql);

	xt->systime_100ns = (unsigned __int64)systime.QuadPart;
	xt->art_cycles = tgpio_art_from_tsc(
		dev, xt->tsc_before + (xt->tsc_after - xt->tsc_before) / 2);
}

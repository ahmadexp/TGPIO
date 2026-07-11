// Licensed under the TGPIO Non-Commercial License (see LICENSE).
// Commercial use requires the prior written permission of Ahmad Byagowi.

/*
 * Intel Time-Aware GPIO -- Windows KMDF driver, IOCTL dispatch.
 * Copyright (c) 2026 Ahmad Byagowi
 *
 * Thin glue between the user-mode interface and the hardware layer. All
 * hardware access runs under the device spin lock; requests are already
 * serialized by the sequential queue, the lock additionally fences the
 * cleanup path.
 */

#include "tgpio.h"

static struct tgpio_win_block *tgpio_block_from_index(
	struct tgpio_win_device *dev, ULONG index)
{
	if (index >= dev->block_count || !dev->block[index].regs)
		return NULL;
	return &dev->block[index];
}

static NTSTATUS tgpio_ioctl_get_info(struct tgpio_win_device *dev,
				     WDFREQUEST request, size_t *written)
{
	struct tgpio_win_info *info;
	NTSTATUS status = WdfRequestRetrieveOutputBuffer(
		request, sizeof(*info), (PVOID *)&info, NULL);
	ULONG i;

	if (!NT_SUCCESS(status))
		return status;

	RtlZeroMemory(info, sizeof(*info));
	info->art_frequency_hz = dev->art_frequency_hz;
	info->tsc_art_numerator = dev->tsc_art_numerator;
	info->tsc_art_denominator = dev->tsc_art_denominator;
	info->tsc_adjust = dev->tsc_adjust;
	info->block_count = dev->block_count;
	for (i = 0; i < dev->block_count; i++)
		info->mmio_base[i] =
			(unsigned __int64)dev->block[i].phys.QuadPart;

	*written = sizeof(*info);
	return STATUS_SUCCESS;
}

static NTSTATUS tgpio_ioctl_crosststamp(struct tgpio_win_device *dev,
					WDFREQUEST request, size_t *written)
{
	struct tgpio_win_crosststamp *xt;
	NTSTATUS status = WdfRequestRetrieveOutputBuffer(
		request, sizeof(*xt), (PVOID *)&xt, NULL);

	if (!NT_SUCCESS(status))
		return status;

	tgpio_art_crosststamp(dev, xt);
	*written = sizeof(*xt);
	return STATUS_SUCCESS;
}

static NTSTATUS tgpio_ioctl_set_input(struct tgpio_win_device *dev,
				      WDFREQUEST request)
{
	struct tgpio_win_set_input *in;
	struct tgpio_win_block *block;
	ULONG edge_bits;
	NTSTATUS status = WdfRequestRetrieveInputBuffer(
		request, sizeof(*in), (PVOID *)&in, NULL);

	if (!NT_SUCCESS(status))
		return status;

	block = tgpio_block_from_index(dev, in->block);
	if (!block)
		return STATUS_INVALID_PARAMETER;

	switch (in->edge) {
	case TGPIO_WIN_EDGE_RISING:
		edge_bits = TGPIOCTL_EP_RISING;
		break;
	case TGPIO_WIN_EDGE_FALLING:
		edge_bits = TGPIOCTL_EP_FALLING;
		break;
	case TGPIO_WIN_EDGE_BOTH:
		edge_bits = TGPIOCTL_EP_TOGGLE;
		break;
	default:
		return STATUS_INVALID_PARAMETER;
	}

	WdfSpinLockAcquire(dev->lock);
	if (block->mode == TGPIO_WIN_BLOCK_OUTPUT)
		tgpio_hw_stop_output(block);
	tgpio_hw_set_input(block, in->enable ? TRUE : FALSE, edge_bits);
	block->mode = in->enable ? TGPIO_WIN_BLOCK_INPUT :
				   TGPIO_WIN_BLOCK_OFF;
	WdfSpinLockRelease(dev->lock);
	return STATUS_SUCCESS;
}

static NTSTATUS tgpio_read_block_index(WDFREQUEST request, ULONG *index)
{
	ULONG *in;
	NTSTATUS status = WdfRequestRetrieveInputBuffer(
		request, sizeof(*in), (PVOID *)&in, NULL);

	if (!NT_SUCCESS(status))
		return status;
	*index = *in;
	return STATUS_SUCCESS;
}

static NTSTATUS tgpio_ioctl_read_capture(struct tgpio_win_device *dev,
					 WDFREQUEST request, size_t *written)
{
	struct tgpio_win_capture *out;
	struct tgpio_win_block *block;
	LARGE_INTEGER systime;
	ULONG index;
	NTSTATUS status = tgpio_read_block_index(request, &index);

	if (!NT_SUCCESS(status))
		return status;
	status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out),
						(PVOID *)&out, NULL);
	if (!NT_SUCCESS(status))
		return status;

	block = tgpio_block_from_index(dev, index);
	if (!block)
		return STATUS_INVALID_PARAMETER;

	WdfSpinLockAcquire(dev->lock);
	tgpio_hw_read_capture(block, &out->event_count, &out->art_cycles);
	out->tsc_now = __rdtsc();
	KeQuerySystemTimePrecise(&systime);
	WdfSpinLockRelease(dev->lock);

	out->systime_100ns = (unsigned __int64)systime.QuadPart;
	*written = sizeof(*out);
	return STATUS_SUCCESS;
}

static NTSTATUS tgpio_ioctl_start_output(struct tgpio_win_device *dev,
					 WDFREQUEST request)
{
	struct tgpio_win_start_output *in;
	struct tgpio_win_block *block;
	unsigned __int64 low_cycles;
	NTSTATUS status = WdfRequestRetrieveInputBuffer(
		request, sizeof(*in), (PVOID *)&in, NULL);

	if (!NT_SUCCESS(status))
		return status;

	block = tgpio_block_from_index(dev, in->block);
	if (!block || !in->high_cycles)
		return STATUS_INVALID_PARAMETER;

	/*
	 * Asymmetric duty needs the per-edge PIV service loop the Linux
	 * driver runs; not ported yet, so only symmetric waveforms here.
	 */
	low_cycles = in->low_cycles ? in->low_cycles : in->high_cycles;
	if (low_cycles != in->high_cycles)
		return STATUS_NOT_SUPPORTED;

	WdfSpinLockAcquire(dev->lock);
	status = tgpio_hw_start_output(dev, block, in->high_cycles,
				       low_cycles, in->first_edge_art);
	block->mode = NT_SUCCESS(status) ? TGPIO_WIN_BLOCK_OUTPUT :
					   TGPIO_WIN_BLOCK_OFF;
	WdfSpinLockRelease(dev->lock);
	return status;
}

static NTSTATUS tgpio_ioctl_stop_output(struct tgpio_win_device *dev,
					WDFREQUEST request)
{
	struct tgpio_win_block *block;
	ULONG index;
	NTSTATUS status = tgpio_read_block_index(request, &index);

	if (!NT_SUCCESS(status))
		return status;
	block = tgpio_block_from_index(dev, index);
	if (!block)
		return STATUS_INVALID_PARAMETER;

	WdfSpinLockAcquire(dev->lock);
	tgpio_hw_stop_output(block);
	block->mode = TGPIO_WIN_BLOCK_OFF;
	WdfSpinLockRelease(dev->lock);
	return STATUS_SUCCESS;
}

static NTSTATUS tgpio_ioctl_invert_output(struct tgpio_win_device *dev,
					  WDFREQUEST request)
{
	struct tgpio_win_block *block;
	ULONG index;
	NTSTATUS status = tgpio_read_block_index(request, &index);

	if (!NT_SUCCESS(status))
		return status;
	block = tgpio_block_from_index(dev, index);
	if (!block)
		return STATUS_INVALID_PARAMETER;

	WdfSpinLockAcquire(dev->lock);
	tgpio_hw_invert_output(block);
	WdfSpinLockRelease(dev->lock);
	return STATUS_SUCCESS;
}

static NTSTATUS tgpio_ioctl_get_block(struct tgpio_win_device *dev,
				      WDFREQUEST request, size_t *written)
{
	struct tgpio_win_block_status *out;
	struct tgpio_win_block *block;
	unsigned __int64 art;
	ULONG index;
	NTSTATUS status = tgpio_read_block_index(request, &index);

	if (!NT_SUCCESS(status))
		return status;
	status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out),
						(PVOID *)&out, NULL);
	if (!NT_SUCCESS(status))
		return status;

	block = tgpio_block_from_index(dev, index);
	if (!block)
		return STATUS_INVALID_PARAMETER;

	WdfSpinLockAcquire(dev->lock);
	tgpio_hw_flop_fold(block);
	out->mode = block->mode;
	out->ctl = tgpio_hw_read_ctl(block);
	out->compv = tgpio_hw_read_compv(block);
	out->piv = tgpio_hw_read_piv(block);
	tgpio_hw_read_capture(block, &out->event_count, &art);
	out->flop_high = block->flop_high;
	out->reserved = 0;
	WdfSpinLockRelease(dev->lock);

	*written = sizeof(*out);
	return STATUS_SUCCESS;
}

VOID tgpio_evt_ioctl(WDFQUEUE queue, WDFREQUEST request,
		     size_t output_buffer_length, size_t input_buffer_length,
		     ULONG io_control_code)
{
	struct tgpio_win_device *dev =
		tgpio_win_get_context(WdfIoQueueGetDevice(queue));
	size_t written = 0;
	NTSTATUS status;

	UNREFERENCED_PARAMETER(output_buffer_length);
	UNREFERENCED_PARAMETER(input_buffer_length);

	switch (io_control_code) {
	case TGPIO_IOCTL_GET_INFO:
		status = tgpio_ioctl_get_info(dev, request, &written);
		break;
	case TGPIO_IOCTL_CROSSTSTAMP:
		status = tgpio_ioctl_crosststamp(dev, request, &written);
		break;
	case TGPIO_IOCTL_SET_INPUT:
		status = tgpio_ioctl_set_input(dev, request);
		break;
	case TGPIO_IOCTL_READ_CAPTURE:
		status = tgpio_ioctl_read_capture(dev, request, &written);
		break;
	case TGPIO_IOCTL_START_OUTPUT:
		status = tgpio_ioctl_start_output(dev, request);
		break;
	case TGPIO_IOCTL_STOP_OUTPUT:
		status = tgpio_ioctl_stop_output(dev, request);
		break;
	case TGPIO_IOCTL_INVERT_OUTPUT:
		status = tgpio_ioctl_invert_output(dev, request);
		break;
	case TGPIO_IOCTL_GET_BLOCK:
		status = tgpio_ioctl_get_block(dev, request, &written);
		break;
	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	WdfRequestCompleteWithInformation(request, status, written);
}

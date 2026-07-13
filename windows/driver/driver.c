// Licensed under the TGPIO Non-Commercial License (see LICENSE).
// Commercial use requires the prior written permission of Ahmad Byagowi.

/*
 * Intel Time-Aware GPIO -- Windows KMDF driver, entry and device setup.
 * Copyright (c) 2026 Ahmad Byagowi
 *
 * Root-enumerated (ROOT\TGPIO): like the Linux reference driver, this does
 * not depend on firmware ACPI enumeration of the TGPIO blocks. The MMIO
 * bases come from the service's Parameters registry key (Addr0Low/High,
 * Addr1Low/High, MmioSize, UseSecond) with the same defaults as the Linux
 * module parameters, and are mapped uncached with MmMapIoSpaceEx.
 */

#include "tgpio.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD tgpio_evt_device_add;
EVT_WDF_DEVICE_CONTEXT_CLEANUP tgpio_evt_device_cleanup;

static ULONG tgpio_reg_read_ulong(WDFKEY key, PCWSTR name, ULONG fallback)
{
	UNICODE_STRING value_name;
	ULONG value;

	RtlInitUnicodeString(&value_name, name);
	if (NT_SUCCESS(WdfRegistryQueryULong(key, &value_name, &value)))
		return value;
	return fallback;
}

static unsigned __int64 tgpio_reg_read_addr(WDFKEY key, PCWSTR low_name,
					    PCWSTR high_name,
					    unsigned __int64 fallback)
{
	ULONG low = tgpio_reg_read_ulong(key, low_name, (ULONG)fallback);
	ULONG high = tgpio_reg_read_ulong(key, high_name,
					  (ULONG)(fallback >> 32));

	return ((unsigned __int64)high << 32) | low;
}

static NTSTATUS tgpio_map_block(struct tgpio_win_block *block,
				unsigned __int64 phys_addr, ULONG size)
{
	block->phys.QuadPart = (LONGLONG)phys_addr;
	block->mmio_size = size;
	block->regs = (volatile UCHAR *)MmMapIoSpaceEx(
		block->phys, size, PAGE_READWRITE | PAGE_NOCACHE);
	if (!block->regs)
		return STATUS_INSUFFICIENT_RESOURCES;
	block->mode = TGPIO_WIN_BLOCK_OFF;
	return STATUS_SUCCESS;
}

NTSTATUS tgpio_evt_device_add(WDFDRIVER driver, PWDFDEVICE_INIT device_init)
{
	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_IO_QUEUE_CONFIG queue_config;
	DECLARE_CONST_UNICODE_STRING(symlink, TGPIO_SYMLINK_NAME);
	struct tgpio_win_device *dev;
	WDFDEVICE wdf_device;
	WDFQUEUE queue;
	WDFKEY params_key = NULL;
	unsigned __int64 addr0 = TGPIO_DEFAULT_ADDR0;
	unsigned __int64 addr1 = TGPIO_DEFAULT_ADDR1;
	ULONG mmio_size = TGPIO_DEFAULT_MMIO_SIZE;
	ULONG use_second = 1;
	ULONG art_frequency_override = 0;
	NTSTATUS status;

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,
						TGPIO_DEVICE_CONTEXT);
	attributes.EvtCleanupCallback = tgpio_evt_device_cleanup;

	status = WdfDeviceCreate(&device_init, &attributes, &wdf_device);
	if (!NT_SUCCESS(status))
		return status;

	dev = tgpio_win_get_context(wdf_device);
	RtlZeroMemory(dev, sizeof(*dev));
	dev->wdf_device = wdf_device;

	status = tgpio_art_detect(dev);
	if (!NT_SUCCESS(status)) {
		KdPrint(("tgpio: no ART on this CPU (CPUID leaf 0x15)\n"));
		return status;
	}

	if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(
		    driver, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES,
		    &params_key))) {
		addr0 = tgpio_reg_read_addr(params_key, L"Addr0Low",
					    L"Addr0High", addr0);
		addr1 = tgpio_reg_read_addr(params_key, L"Addr1Low",
					    L"Addr1High", addr1);
		mmio_size = tgpio_reg_read_ulong(params_key, L"MmioSize",
						 mmio_size);
		use_second = tgpio_reg_read_ulong(params_key, L"UseSecond",
						  use_second);
		/* Crystal frequency when CPUID 15h leaves ECX zero. */
		art_frequency_override = tgpio_reg_read_ulong(
			params_key, L"ArtFrequencyHz", 0);
		if (art_frequency_override)
			dev->art_frequency_hz = art_frequency_override;
		WdfRegistryClose(params_key);
	}
	if (!dev->art_frequency_hz) {
		KdPrint(("tgpio: CPUID 15h has no crystal frequency; set "
			 "the ArtFrequencyHz registry parameter\n"));
		return STATUS_NOT_SUPPORTED;
	}
	KdPrint(("tgpio: ART %llu Hz, TSC/ART %lu/%lu, tsc_adjust %llu\n",
		 dev->art_frequency_hz, dev->tsc_art_numerator,
		 dev->tsc_art_denominator, dev->tsc_adjust));
	if (mmio_size < TGPIO_DEFAULT_MMIO_SIZE)
		mmio_size = TGPIO_DEFAULT_MMIO_SIZE;

	status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &dev->lock);
	if (!NT_SUCCESS(status))
		return status;

	status = tgpio_map_block(&dev->block[0], addr0, mmio_size);
	if (!NT_SUCCESS(status))
		return status;
	dev->block_count = 1;

	if (use_second) {
		status = tgpio_map_block(&dev->block[1], addr1, mmio_size);
		if (!NT_SUCCESS(status))
			return status; /* cleanup unmaps block 0 */
		dev->block_count = 2;
	}

	KdPrint(("tgpio: mapped %lu block(s) at 0x%llx / 0x%llx size 0x%lx\n",
		 dev->block_count, addr0, use_second ? addr1 : 0, mmio_size));

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_config,
					       WdfIoQueueDispatchSequential);
	queue_config.EvtIoDeviceControl = tgpio_evt_ioctl;
	status = WdfIoQueueCreate(wdf_device, &queue_config,
				  WDF_NO_OBJECT_ATTRIBUTES, &queue);
	if (!NT_SUCCESS(status))
		return status;

	return WdfDeviceCreateSymbolicLink(wdf_device, &symlink);
}

VOID tgpio_evt_device_cleanup(WDFOBJECT object)
{
	struct tgpio_win_device *dev = tgpio_win_get_context(object);
	ULONG i;

	for (i = 0; i < TGPIO_WIN_MAX_BLOCKS; i++) {
		struct tgpio_win_block *block = &dev->block[i];

		if (!block->regs)
			continue;
		/* Leave the pins quiet across unload. */
		if (block->mode == TGPIO_WIN_BLOCK_OUTPUT)
			tgpio_hw_stop_output(block);
		else if (block->mode == TGPIO_WIN_BLOCK_INPUT)
			tgpio_hw_set_input(block, FALSE, 0);
		MmUnmapIoSpace((PVOID)block->regs, block->mmio_size);
		block->regs = NULL;
	}
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object,
		     PUNICODE_STRING registry_path)
{
	WDF_DRIVER_CONFIG config;

	WDF_DRIVER_CONFIG_INIT(&config, tgpio_evt_device_add);
	return WdfDriverCreate(driver_object, registry_path,
			       WDF_NO_OBJECT_ATTRIBUTES, &config,
			       WDF_NO_HANDLE);
}

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
EVT_WDF_DEVICE_PREPARE_HARDWARE tgpio_evt_prepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE tgpio_evt_release;
EVT_WDF_DEVICE_D0_ENTRY tgpio_evt_d0_entry;
EVT_WDF_DEVICE_D0_EXIT tgpio_evt_d0_exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL tgpio_evt_surprise_removal;

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

NTSTATUS tgpio_evt_device_add(WDFDRIVER driver, PWDFDEVICE_INIT device_init)
{
	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_IO_QUEUE_CONFIG queue_config;
	WDF_PNPPOWER_EVENT_CALLBACKS power;
	DECLARE_CONST_UNICODE_STRING(symlink, TGPIO_SYMLINK_NAME);
	struct tgpio_win_device *dev;
	WDFDEVICE wdf_device;
	WDFQUEUE queue;
	WDFKEY params_key = NULL;
	unsigned __int64 addr0 = TGPIO_DEFAULT_ADDR0;
	unsigned __int64 addr1 = TGPIO_DEFAULT_ADDR1;
	ULONG mmio_size = TGPIO_DEFAULT_MMIO_SIZE;
	ULONG use_second = 1;
	NTSTATUS status;

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,
						TGPIO_DEVICE_CONTEXT);
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
	power.EvtDevicePrepareHardware = tgpio_evt_prepare;
	power.EvtDeviceReleaseHardware = tgpio_evt_release;
	power.EvtDeviceD0Entry = tgpio_evt_d0_entry;
	power.EvtDeviceD0Exit = tgpio_evt_d0_exit;
	power.EvtDeviceSurpriseRemoval = tgpio_evt_surprise_removal;
	WdfDeviceInitSetPnpPowerEventCallbacks(device_init, &power);

	status = WdfDeviceCreate(&device_init, &attributes, &wdf_device);
	if (!NT_SUCCESS(status))
		return status;

	dev = tgpio_win_get_context(wdf_device);
	RtlZeroMemory(dev, sizeof(*dev));
	dev->wdf_device = wdf_device;

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
		dev->art_frequency_override = tgpio_reg_read_ulong(
			params_key, L"ArtFrequencyHz", 0);
		WdfRegistryClose(params_key);
	}
	if (mmio_size < TGPIO_DEFAULT_MMIO_SIZE)
		mmio_size = TGPIO_DEFAULT_MMIO_SIZE;

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = wdf_device;
	status = WdfSpinLockCreate(&attributes, &dev->lock);
	if (!NT_SUCCESS(status))
		return status;

	dev->block_count = use_second ? 2 : 1;
	dev->block[0].phys.QuadPart = (LONGLONG)addr0;
	dev->block[0].mmio_size = mmio_size;
	dev->block[1].phys.QuadPart = (LONGLONG)addr1;
	dev->block[1].mmio_size = mmio_size;

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_config,
					       WdfIoQueueDispatchSequential);
	queue_config.PowerManaged = WdfTrue;
	queue_config.EvtIoDeviceControl = tgpio_evt_ioctl;
	status = WdfIoQueueCreate(wdf_device, &queue_config,
				  WDF_NO_OBJECT_ATTRIBUTES, &queue);
	if (!NT_SUCCESS(status))
		return status;

	return WdfDeviceCreateSymbolicLink(wdf_device, &symlink);
}

NTSTATUS tgpio_evt_prepare(WDFDEVICE device, WDFCMRESLIST raw,
			   WDFCMRESLIST translated)
{
	struct tgpio_win_device *dev = tgpio_win_get_context(device);
	ULONG i;

	/* ROOT\TGPIO has no translated resources. Its pinned board profile
	 * supplies the two addresses, as on the legacy root installation. */
	UNREFERENCED_PARAMETER(raw);
	UNREFERENCED_PARAMETER(translated);
	for (i = 0; i < dev->block_count; ++i) {
		struct tgpio_win_block *block = &dev->block[i];

		block->regs = MmMapIoSpaceEx(block->phys, block->mmio_size,
					     PAGE_READWRITE | PAGE_NOCACHE);
		if (!block->regs)
			return STATUS_INSUFFICIENT_RESOURCES;
	}
	return STATUS_SUCCESS;
}

NTSTATUS tgpio_evt_release(WDFDEVICE device, WDFCMRESLIST translated)
{
	struct tgpio_win_device *dev = tgpio_win_get_context(device);
	ULONG i;

	UNREFERENCED_PARAMETER(translated);
	/* KMDF also calls this after a partially successful prepare. */
	for (i = 0; i < dev->block_count; ++i) {
		struct tgpio_win_block *block = &dev->block[i];

		if (block->regs) {
			MmUnmapIoSpace((PVOID)block->regs, block->mmio_size);
			block->regs = NULL;
		}
	}
	return STATUS_SUCCESS;
}

NTSTATUS tgpio_evt_d0_entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
	struct tgpio_win_device *dev = tgpio_win_get_context(device);
	NTSTATUS status;
	ULONG i;

	UNREFERENCED_PARAMETER(previous);
	status = tgpio_art_detect(dev);
	if (!NT_SUCCESS(status))
		return status;
	if (dev->art_frequency_override)
		dev->art_frequency_hz = dev->art_frequency_override;
	if (!dev->art_frequency_hz)
		return STATUS_NOT_SUPPORTED;
	WdfSpinLockAcquire(dev->lock);
	if (dev->removed) {
		WdfSpinLockRelease(dev->lock);
		return STATUS_DEVICE_REMOVED;
	}
	for (i = 0; i < dev->block_count; ++i)
		tgpio_hw_resume(&dev->block[i]);
	WdfSpinLockRelease(dev->lock);
	return STATUS_SUCCESS;
}

NTSTATUS tgpio_evt_d0_exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE next)
{
	struct tgpio_win_device *dev = tgpio_win_get_context(device);
	ULONG i;

	UNREFERENCED_PARAMETER(next);
	WdfSpinLockAcquire(dev->lock);
	if (!dev->removed) {
		for (i = 0; i < dev->block_count; ++i)
			tgpio_hw_suspend(&dev->block[i]);
	}
	WdfSpinLockRelease(dev->lock);
	return STATUS_SUCCESS;
}

VOID tgpio_evt_surprise_removal(WDFDEVICE device)
{
	struct tgpio_win_device *dev = tgpio_win_get_context(device);

	WdfSpinLockAcquire(dev->lock);
	dev->removed = TRUE;
	WdfSpinLockRelease(dev->lock);
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

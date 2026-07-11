/* Minimal kernel stubs for host-side unit tests of hw.c/art.c. */
#ifndef STUB_NTDDK_H
#define STUB_NTDDK_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

typedef unsigned long ULONG;
typedef unsigned char UCHAR, BOOLEAN, KIRQL;
typedef long NTSTATUS;
typedef void VOID;
typedef void *PVOID;
typedef const wchar_t *PCWSTR;
typedef union { long long QuadPart; } LARGE_INTEGER;
typedef union { long long QuadPart; } PHYSICAL_ADDRESS;
#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_SUCCESS 0L
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#define STATUS_INTEGER_OVERFLOW ((NTSTATUS)0xC0000095L)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010L)
#define DISPATCH_LEVEL 2
#define KdPrint(x) printf x

/* Register write log so tests can assert programming order. */
#define REGLOG_MAX 256
struct reglog_entry { int is_write; ULONG offset; ULONG value; };
extern struct reglog_entry reglog[REGLOG_MAX];
extern int reglog_count;
extern UCHAR fake_mmio[0x40];

static inline ULONG stub_read_reg(volatile ULONG *reg)
{
	ULONG off = (ULONG)((UCHAR *)(uintptr_t)reg - fake_mmio);
	uint32_t v;
	memcpy(&v, fake_mmio + off, 4);
	if (reglog_count < REGLOG_MAX)
		reglog[reglog_count++] = (struct reglog_entry){ 0, off, v };
	return v;
}
static inline void stub_write_reg(volatile ULONG *reg, ULONG value)
{
	ULONG off = (ULONG)((UCHAR *)(uintptr_t)reg - fake_mmio);
	uint32_t v32 = (uint32_t)value;
	memcpy(fake_mmio + off, &v32, 4);
	if (reglog_count < REGLOG_MAX)
		reglog[reglog_count++] = (struct reglog_entry){ 1, off, value };
}
#define READ_REGISTER_ULONG(r) stub_read_reg(r)
#define WRITE_REGISTER_ULONG(r, v) stub_write_reg((r), (v))

extern unsigned long long fake_systime_100ns;
static inline void KeQuerySystemTimePrecise(LARGE_INTEGER *t)
{ t->QuadPart = (long long)fake_systime_100ns; }
static inline void KeRaiseIrql(KIRQL level, KIRQL *old) { (void)level; *old = 0; }
static inline void KeLowerIrql(KIRQL irql) { (void)irql; }
#endif

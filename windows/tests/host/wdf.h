#ifndef STUB_WDF_H
#define STUB_WDF_H
typedef void *WDFDEVICE, *WDFSPINLOCK, *WDFQUEUE, *WDFREQUEST, *WDFKEY, *WDFDRIVER, *WDFOBJECT;
#define WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(type, fn) \
	type *fn##_unused_decl(void);
typedef void EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL(WDFQUEUE, WDFREQUEST,
						size_t, size_t, ULONG);
#include <stddef.h>
#endif

/* Licensed under the TGPIO Non-Commercial License (see LICENSE).
 * Commercial use requires the prior written permission of Ahmad Byagowi. */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM tgpio

#if !defined(_TGPIO_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TGPIO_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(tgpio_timestamp_fallback,

	TP_PROTO(unsigned int block_index, u64 art_cycles),

	TP_ARGS(block_index, art_cycles),

	TP_STRUCT__entry(
		__field(unsigned int,	block_index)
		__field(u64,		art_cycles)
	),

	TP_fast_assign(
		__entry->block_index = block_index;
		__entry->art_cycles = art_cycles;
	),

	TP_printk("block=%u art=%llu",
		  __entry->block_index, __entry->art_cycles)
);

TRACE_EVENT(tgpio_phc_step,

	TP_PROTO(s64 old_ns, s64 new_ns),

	TP_ARGS(old_ns, new_ns),

	TP_STRUCT__entry(
		__field(s64,	old_ns)
		__field(s64,	new_ns)
	),

	TP_fast_assign(
		__entry->old_ns = old_ns;
		__entry->new_ns = new_ns;
	),

	TP_printk("old_ns=%lld new_ns=%lld delta_ns=%lld",
		  __entry->old_ns, __entry->new_ns,
		  __entry->new_ns - __entry->old_ns)
);

TRACE_EVENT(tgpio_base_clock_lost,

	TP_PROTO(unsigned int block_index),

	TP_ARGS(block_index),

	TP_STRUCT__entry(
		__field(unsigned int,	block_index)
	),

	TP_fast_assign(
		__entry->block_index = block_index;
	),

	TP_printk("block=%u", __entry->block_index)
);

#endif /* _TGPIO_TRACE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE tgpio-trace
#include <trace/define_trace.h>

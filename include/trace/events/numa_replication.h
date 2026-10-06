/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM numa_replication

#if !defined(_TRACE_NUMA_REPL_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_NUMA_REPL_H

#include <linux/mm.h>
#include <linux/tracepoint.h>

#define repl_pfn_node(pfn) (pfn_valid(pfn) ? pfn_to_nid(pfn) : NUMA_NO_NODE)

/* clang-format off */
DECLARE_EVENT_CLASS(repl,
	TP_PROTO(int node, unsigned long addr, unsigned long pfn),
	TP_ARGS(node, addr, pfn),
	TP_STRUCT__entry(
		__field(int, node)
		__field(int, cpu_node)
		__field(unsigned long, addr)
		__field(unsigned long, pfn)
		__field(int, pfn_node)
	),
	TP_fast_assign(
		__entry->node = node;
		__entry->cpu_node = numa_node_id();
		__entry->addr = addr;
		__entry->pfn = pfn;
		__entry->pfn_node = repl_pfn_node(pfn);
	),
	TP_printk("n%d %lx -> %lx(n%d)", __entry->node, __entry->addr,
		  __entry->pfn, __entry->pfn_node)
);

#define DEFINE_REPL_EVENT(name)						\
DEFINE_EVENT(repl, name,						\
	TP_PROTO(int node, unsigned long addr, unsigned long pfn),	\
	TP_ARGS(node, addr, pfn))

#define DEFINE_REPL_REMOTE_EVENT(name)					\
DEFINE_EVENT_PRINT(repl, name,						\
	TP_PROTO(int node, unsigned long addr, unsigned long pfn),	\
	TP_ARGS(node, addr, pfn),					\
	TP_printk("n%d %lx -> %lx(n%d) by n%d", __entry->node,		\
		  __entry->addr, __entry->pfn, __entry->pfn_node,	\
		  __entry->cpu_node))

DEFINE_REPL_EVENT(repl_install);
DEFINE_REPL_EVENT(repl_share);
DEFINE_REPL_EVENT(repl_refresh);
DEFINE_REPL_EVENT(repl_write);
DEFINE_REPL_REMOTE_EVENT(repl_main_fill);
DEFINE_REPL_REMOTE_EVENT(repl_main_swapin);
DEFINE_REPL_REMOTE_EVENT(repl_main_zap);
DEFINE_REPL_REMOTE_EVENT(repl_invalidate);
DEFINE_REPL_REMOTE_EVENT(repl_free);

TRACE_EVENT(repl_copy,
	TP_PROTO(unsigned long addr, unsigned long src, unsigned long dst),
	TP_ARGS(addr, src, dst),
	TP_STRUCT__entry(
		__field(int, cpu_node)
		__field(unsigned long, addr)
		__field(unsigned long, src)
		__field(int, src_node)
		__field(unsigned long, dst)
		__field(int, dst_node)
	),
	TP_fast_assign(
		__entry->cpu_node = numa_node_id();
		__entry->addr = addr;
		__entry->src = src;
		__entry->src_node = repl_pfn_node(src);
		__entry->dst = dst;
		__entry->dst_node = repl_pfn_node(dst);
	),
	TP_printk("n%d %lx %lx(n%d) -> %lx(n%d)", __entry->cpu_node,
		  __entry->addr, __entry->src, __entry->src_node,
		  __entry->dst, __entry->dst_node)
);
/* clang-format on */

#endif

#include <trace/define_trace.h>

/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PERF_MAKO_H
#define PERF_MAKO_H

#include <linux/types.h>

/* perf_asm.S */
unsigned long perf_get_dr2(void);
void perf_set_dr2(unsigned long dr2);
uint64_t perf_mako_rdr_read(unsigned long index);
void perf_mako_rdr_write(unsigned long index, uint64_t value);
void perf_intrigue_enable_perf_counters(void);
void perf_intrigue_disable_perf_counters(void);

/*
 * Event sets, which select what ctr1, ctr2 and ctr3 count. Counters are
 * listed ctr0 to ctr3.
 *
 * DEFAULT:     TLB misses, cycles, instructions retired, cycles
 * STALL:       TLB misses, stall cycles, 0, stall cycles
 * NONSTALL:    TLB misses, non-stall cycles, instructions retired in
 *              non-stall cycles, non-stall cycles
 * TLB_HANDLER: TLB misses, cycles in TLB miss handling, about one per
 *              miss, cycles in TLB miss handling. Only on the first core
 *              of a processor.
 */
enum {
	MAKO_EVENTS_DEFAULT,
	MAKO_EVENTS_STALL,
	MAKO_EVENTS_NONSTALL,
	MAKO_EVENTS_TLB_HANDLER,
};

/* What ctr0 counts */
enum {
	MAKO_CTR0_TLB,		/* TLB misses, instruction and data */
	MAKO_CTR0_DTLB,		/* data TLB misses */
	MAKO_CTR0_ITLB,		/* instruction TLB misses */
	MAKO_CTR0_INSNS,	/* instructions retired and not nullified */
	MAKO_CTR0_STORES,	/* stores retired */
};

/* What ctr2 counts, in the default event set */
enum {
	MAKO_CTR2_INSNS,	/* instructions retired, maybe filtered */
	MAKO_CTR2_MEMOPS,	/* loads and stores, few of those that miss */
	MAKO_CTR2_DMISSES,	/* loads and stores that miss the D-cache */
	MAKO_CTR2_STORE_DMISSES, /* stores that miss the D-cache */
	MAKO_CTR2_BRANCHES,	/* branches retired */
	MAKO_CTR2_TAKEN,	/* taken branches retired */
};

/*
 * With filter set, ctr2 counts only the retired instructions whose bits
 * selected by mask equal those of match, or with invert all other
 * instructions. Nullified instructions count.
 */
struct perf_mako_config {
	unsigned int	set;		/* MAKO_EVENTS_* */
	unsigned int	ctr0;		/* MAKO_CTR0_* */
	unsigned int	ctr2;		/* MAKO_CTR2_* */
	bool		filter;		/* filter MAKO_CTR2_INSNS */
	bool		invert;
	uint32_t	mask;
	uint32_t	match;
};

/* perf_mako.c, with interrupts off */
int perf_mako_load(const struct perf_mako_config *cfg);
int perf_mako_read(uint32_t *c);
void perf_mako_enable(void);
void perf_mako_disable(void);
bool perf_mako_is_loaded(const struct perf_mako_config *cfg);
bool perf_mako_core_b(void);
int perf_mako_chip_cpu(int cpu);

/* Exclusion between /dev/perf and perf events */
bool perf_mako_dev_get(void);
void perf_mako_dev_put(void);
bool perf_mako_pmu_get(void);
void perf_mako_pmu_put(void);

/* perf_mako.c, for /dev/perf */
ssize_t perf_mako_write(const char __user *buf, size_t count);
int perf_mako_start(void);
int perf_mako_stop(uint32_t *raddr);

#endif

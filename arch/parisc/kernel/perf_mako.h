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

/* What ctr0 counts */
enum {
	MAKO_CTR0_TLB,		/* TLB misses, instruction and data */
	MAKO_CTR0_DTLB,		/* data TLB misses */
	MAKO_CTR0_ITLB,		/* instruction TLB misses */
	MAKO_CTR0_INSNS,	/* instructions retired and not nullified */
};

struct perf_mako_config {
	unsigned int	ctr0;		/* MAKO_CTR0_* */
};

/* perf_mako.c, with interrupts off */
int perf_mako_load(const struct perf_mako_config *cfg);
int perf_mako_read(uint32_t *c);
void perf_mako_enable(void);
void perf_mako_disable(void);
bool perf_mako_is_loaded(const struct perf_mako_config *cfg);

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

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

/* perf_mako.c, for /dev/perf */
ssize_t perf_mako_write(const char __user *buf, size_t count);
int perf_mako_start(void);
int perf_mako_stop(uint32_t *raddr);

#endif

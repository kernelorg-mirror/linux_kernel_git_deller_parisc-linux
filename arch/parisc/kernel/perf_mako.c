// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PA-8800/PA-8900 (Mako) performance counters
 *
 * Each core has four counters in RDR15 of RDR chain 1, which the two cores
 * of a processor share: the first four 32-bit words belong to the core
 * with DR2 bit 54 clear. A counter is 31 bits wide; bit 31 is set when it
 * wraps and stays set. Before shifting chain 1, a core requests it by setting
 * DR2 bit 48 and waits for the grant in DR2 bit 53. Writes to both chains
 * are bracketed by setting and clearing bit 56 of RDR9 on chain 1. The
 * order and the image are those of the C8000 PDC 2.13 firmware's
 * PDC_INTRIGUE implementation.
 *
 * pmenb before an image has been written takes an HPMC, so the counters
 * are only started on a CPU once the image has been loaded there.
 */

#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/uaccess.h>
#include <asm/perf.h>
#include <asm/special_insns.h>

#include "perf_mako.h"
#include "perf_mako_image.h"

#define MAKO_RDR(rdr, chain)	((rdr) | (chain) << 5)
#define MAKO_DR2_CHAIN1_REQ	(1UL << 48)
#define MAKO_DR2_CHAIN1_GRANT	(1UL << 53)
#define MAKO_DR2_CORE		(1UL << 54)
#define MAKO_RDR9_1_CONFIG	(1ULL << 56)
#define MAKO_CCR_PERF		0x20

static DEFINE_PER_CPU(bool, perf_mako_loaded);

/* The second core of a processor, which owns the second half of RDR15_1. */
static bool perf_mako_core_b(void)
{
	return perf_get_dr2() & MAKO_DR2_CORE;
}

/*
 * The other core holds chain 1 only while it loads or reads its counters,
 * with interrupts off.
 */
static int perf_mako_chain1_get(void)
{
	unsigned long dr2;
	int ret;

	perf_set_dr2(perf_get_dr2() | MAKO_DR2_CHAIN1_REQ);
	ret = read_poll_timeout_atomic(perf_get_dr2, dr2,
				       dr2 & MAKO_DR2_CHAIN1_GRANT, 1,
				       USEC_PER_MSEC, false);
	if (ret)
		perf_set_dr2(perf_get_dr2() & ~MAKO_DR2_CHAIN1_REQ);

	return ret;
}

static void perf_mako_chain1_put(void)
{
	perf_set_dr2(perf_get_dr2() & ~MAKO_DR2_CHAIN1_REQ);
}

/* The last doubleword is shifted first, for both reads and writes. */
static void perf_mako_rdr_read_buf(unsigned int index, uint64_t *buf,
				   unsigned int n)
{
	while (n--)
		buf[n] = perf_mako_rdr_read(index);
}

static void perf_mako_rdr_write_buf(unsigned int index, const uint64_t *buf,
				    unsigned int n)
{
	while (n--)
		perf_mako_rdr_write(index, buf[n]);
}

/*
 * Write one RDR of the image. Where the image has a mask, merge it with
 * the current contents the way the firmware does, new = (image & ~mask) |
 * (current & mask). A read returns the RDR's bits right-justified in
 * num_words doublewords, while the image and the mask are left-justified,
 * so the read is shifted left by the unused bits.
 */
static void perf_mako_rdr_merge(const struct perf_mako_rdr *r,
				const uint64_t *data)
{
	unsigned int idx = MAKO_RDR(r->rdr, r->chain), n = r->num_words, s, j;
	uint64_t buf[4];

	if (!r->mask || WARN_ON_ONCE(n > ARRAY_SIZE(buf))) {
		perf_mako_rdr_write_buf(idx, data, n);
		return;
	}

	s = r->width % 64 ? 64 - r->width % 64 : 0;
	perf_mako_rdr_read_buf(idx, buf, n);
	for (j = 0; s && j < n; j++)
		buf[j] = buf[j] << s | (j + 1 < n ? buf[j + 1] >> (64 - s) : 0);
	for (j = 0; j < n; j++)
		buf[j] = (data[j] & ~r->mask[j]) | (buf[j] & r->mask[j]);
	perf_mako_rdr_write_buf(idx, buf, n);
}

static void perf_mako_config_bit(bool on)
{
	uint64_t val = perf_mako_rdr_read(MAKO_RDR(9, 1));

	if (on)
		val |= MAKO_RDR9_1_CONFIG;
	else
		val &= ~MAKO_RDR9_1_CONFIG;
	perf_mako_rdr_write(MAKO_RDR(9, 1), val);
}

/*
 * Read RDR15_1, optionally clearing this core's counters, and write it
 * back. Reads shift the RDR out, and the other core's counters must be
 * kept.
 */
static void perf_mako_ctrs_rw(uint64_t *buf, bool clear)
{
	perf_mako_rdr_read_buf(MAKO_RDR(15, 1), buf, 4);
	if (clear) {
		unsigned int h = perf_mako_core_b() ? 2 : 0;

		buf[h] = 0;
		buf[h + 1] = 0;
	}
	perf_mako_rdr_write_buf(MAKO_RDR(15, 1), buf, 4);
}

/*
 * Write the image and start the counters on this CPU, from zero. Called
 * with interrupts off. Like the firmware's boot-time load, this holds
 * chain 1 across the image write and pmenb.
 */
static int perf_mako_load(void)
{
	unsigned long ccr = mfctl(10);
	uint64_t ctrs[4];
	int i, ret;

	mtctl(ccr | MAKO_CCR_PERF, 10);
	ret = perf_mako_chain1_get();
	if (ret)
		goto out;

	perf_mako_config_bit(true);
	for (i = 0; i < ARRAY_SIZE(perf_mako_image); i++) {
		const struct perf_mako_rdr *r = &perf_mako_image[i];

		/*
		 * The firmware presets the first counter to 0x7fffffec.
		 * Start this core's counters from zero instead, and keep
		 * the other core's.
		 */
		if (r->chain == 1 && r->rdr == 15)
			perf_mako_ctrs_rw(ctrs, true);
		else
			perf_mako_rdr_merge(r, r->data);
	}
	perf_mako_config_bit(false);

	asm volatile("pmenb" ::: "memory");
	perf_mako_chain1_put();
	this_cpu_write(perf_mako_loaded, true);
out:
	mtctl(ccr, 10);
	return ret;
}

/* Read this CPU's four counters. Called with interrupts off. */
static int perf_mako_read(uint32_t *c)
{
	unsigned long ccr = mfctl(10);
	uint64_t buf[4];
	int i, w, ret;

	mtctl(ccr | MAKO_CCR_PERF, 10);
	ret = perf_mako_chain1_get();
	if (ret)
		goto out;

	perf_mako_config_bit(true);
	perf_mako_ctrs_rw(buf, false);
	perf_mako_config_bit(false);
	perf_mako_chain1_put();

	w = perf_mako_core_b() ? 4 : 0;
	for (i = 0; i < 4; i++, w++)
		c[i] = buf[w / 2] >> (w % 2 ? 0 : 32);
out:
	mtctl(ccr, 10);
	return ret;
}

int perf_mako_start(void)
{
	unsigned long flags;
	int ret = 0;

	local_irq_save(flags);
	if (this_cpu_read(perf_mako_loaded))
		perf_intrigue_enable_perf_counters();
	else
		ret = perf_mako_load();
	local_irq_restore(flags);

	return ret;
}

int perf_mako_stop(uint32_t *raddr)
{
	unsigned long flags;
	int ret = -EINVAL;

	local_irq_save(flags);
	if (this_cpu_read(perf_mako_loaded)) {
		perf_intrigue_disable_perf_counters();
		ret = perf_mako_read(raddr);
	}
	local_irq_restore(flags);

	return ret;
}

/*
 * The only image is (CUDA_INTF << 16) | 0: ctr0 counts TLB misses, ctr1
 * and ctr3 cycles, and ctr2 instructions retired. Writing it loads the
 * image on this CPU and restarts its counters from zero.
 */
ssize_t perf_mako_write(const char __user *buf, size_t count)
{
	unsigned long flags;
	uint32_t image;
	int ret;

	if (count != sizeof(image))
		return -EIO;
	if (copy_from_user(&image, buf, sizeof(image)))
		return -EFAULT;
	if (image != CUDA_INTF << 16)
		return -EINVAL;

	local_irq_save(flags);
	if (this_cpu_read(perf_mako_loaded))
		perf_intrigue_disable_perf_counters();
	ret = perf_mako_load();
	local_irq_restore(flags);

	return ret ? ret : count;
}

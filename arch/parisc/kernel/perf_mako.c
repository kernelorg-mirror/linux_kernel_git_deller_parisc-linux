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

#include <linux/bitfield.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/topology.h>
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

/*
 * The cycle signal of ctr1, ctr3 and ctr2 can be qualified by stall
 * cycles: RDR16_1 bit 291 does this for the first core of a processor and
 * bit 275 for the second. Each garbles the other core's counts, so only
 * the bit of the core being programmed is set. Clearing bit 559 qualifies
 * the signal by non-stall cycles on the first core; on the second it stops
 * all four counters. There, setting bit 324 together with bit 275 turns
 * the stall qualifier of ctr1 and ctr3 into a non-stall one, and bit 21
 * then qualifies ctr2 the same way.
 *
 * Setting RDR16_1 bit 408 and clearing bit 415 makes ctr1 and ctr3 count
 * the cycles spent handling TLB misses, and ctr2 about one per miss. This
 * works only on the first core; on the second it stops all four counters.
 *
 * RDR4_0 compares each retiring instruction against a match value under a
 * mask and gates ctr2 with the result. Instruction bit j (0 is the most
 * significant) has its mask bit at 145 - j and its match bit at 108 + j
 * for j <= 18, and at 282 - j and 205 + j above that. A set mask bit
 * means don't care. Bit 19 inverts the comparison.
 *
 * RDR4_0 bits 0-7 select what ctr0 counts. With bit 3 clear it counts TLB
 * misses; bit 6 leaves out the data TLB misses and bit 7 the instruction
 * TLB misses. With bit 3 set it counts retired instructions that are not
 * nullified, and bits 4-7 each leave out the instructions at one of the
 * four word offsets in an aligned 16-byte block. With bit 3 set, RDR0_0
 * bit 11 makes it count stores instead.
 *
 * RDR0_0 bits 0-4 select what ctr2 counts: instructions (the image's
 * 01000), loads and stores (00100) or loads and stores that miss the
 * D-cache (00101). The loads and stores count leaves out most of the
 * accesses that miss. Bit 13 narrows either count to stores.
 *
 * The match filter set to opcodes 0x20-0x3f, with RDR4_0 bit 247 clear and
 * bits 266, 268 and 271-276 set, counts branches on ctr2. RDR4_0 bytes
 * 0-3 set to a0 80 80 a0 count taken branches. Both settings come from the
 * PCX-W (PA-8500 to PA-8700) branch images.
 *
 * RDR bits are numbered from the most significant bit of the first
 * doubleword.
 */
#define MAKO_RDR16_1_STALL_A	291
#define MAKO_RDR16_1_STALL_B	275
#define MAKO_RDR16_1_NONSTALL	559
#define MAKO_RDR16_1_NONSTALL_B	324
#define MAKO_RDR16_1_NONSTALL_B_CTR2	21
#define MAKO_RDR16_1_TLB_HANDLER	408
#define MAKO_RDR16_1_TLB_HANDLER_CLR	415
#define MAKO_RDR4_0_INVERT	19
#define MAKO_RDR4_0_CTR0_INSNS	3
#define MAKO_RDR4_0_CTR0_NO_DTLB	6
#define MAKO_RDR4_0_CTR0_NO_ITLB	7
#define MAKO_RDR4_0_BRANCH	247
#define MAKO_RDR4_0_TAKEN	0xa08080a0U
#define MAKO_RDR0_0_CLASS	GENMASK_ULL(63, 59)
#define MAKO_RDR0_0_MEMOPS	0x04
#define MAKO_RDR0_0_DMISSES	0x05
#define MAKO_RDR0_0_CTR0_STORES	11
#define MAKO_RDR0_0_CTR2_STORES	13

static const unsigned int perf_mako_rdr4_0_branch_set[] = {
	266, 268, 271, 272, 273, 274, 275, 276,
};

struct perf_mako_cpu {
	bool			loaded;
	struct perf_mako_config	cfg;
	uint64_t		rdr0_0[ARRAY_SIZE(perf_mako_rdr0_0)];
	uint64_t		rdr4_0[ARRAY_SIZE(perf_mako_rdr4_0)];
	uint64_t		rdr16_1[ARRAY_SIZE(perf_mako_rdr16_1)];
};

static DEFINE_PER_CPU(struct perf_mako_cpu, perf_mako_cpu);

/* /dev/perf loads the default configuration. */
static const struct perf_mako_config perf_mako_dev_cfg;

/*
 * RDR16_1 is shared by the two cores of a processor, and each load writes
 * all of it, so a load on one core undoes the event set the other core
 * selected. Record per processor, at perf_mako_chip_cpu(), which core's
 * set RDR16_1 holds: the default set is the same for both cores. Loads
 * are serialized by the chain 1 handshake.
 */
static DEFINE_PER_CPU(int, perf_mako_rdr16_1_key);

/* /dev/perf and perf events must not program the counters at once. */
static DEFINE_SPINLOCK(perf_mako_owner_lock);
static bool perf_mako_dev_open;
static unsigned int perf_mako_pmu_users;

bool perf_mako_dev_get(void)
{
	bool ok;

	spin_lock(&perf_mako_owner_lock);
	ok = !perf_mako_pmu_users;
	if (ok)
		perf_mako_dev_open = true;
	spin_unlock(&perf_mako_owner_lock);

	return ok;
}

void perf_mako_dev_put(void)
{
	spin_lock(&perf_mako_owner_lock);
	perf_mako_dev_open = false;
	spin_unlock(&perf_mako_owner_lock);
}

bool perf_mako_pmu_get(void)
{
	bool ok;

	spin_lock(&perf_mako_owner_lock);
	ok = !perf_mako_dev_open;
	if (ok)
		perf_mako_pmu_users++;
	spin_unlock(&perf_mako_owner_lock);

	return ok;
}

void perf_mako_pmu_put(void)
{
	spin_lock(&perf_mako_owner_lock);
	perf_mako_pmu_users--;
	spin_unlock(&perf_mako_owner_lock);
}

/* The second core of a processor, which owns the second half of RDR15_1. */
bool perf_mako_core_b(void)
{
	return perf_get_dr2() & MAKO_DR2_CORE;
}

/*
 * The first CPU of cpu's processor, where data shared by the processor's
 * cores is kept. The package id stays the same while a CPU is offline.
 */
int perf_mako_chip_cpu(int cpu)
{
	int id = topology_physical_package_id(cpu), c;

	for_each_possible_cpu(c)
		if (topology_physical_package_id(c) == id)
			return c;

	return cpu;
}

static int perf_mako_key(const struct perf_mako_config *cfg)
{
	if (cfg->set == MAKO_EVENTS_DEFAULT)
		return 0;
	return cfg->set | perf_mako_core_b() << 4;
}

static int *perf_mako_chip_key(void)
{
	return per_cpu_ptr(&perf_mako_rdr16_1_key,
			   perf_mako_chip_cpu(smp_processor_id()));
}

static void perf_mako_set_bit(uint64_t *buf, unsigned int bit, bool val)
{
	uint64_t m = 1ULL << (63 - bit % 64);

	if (val)
		buf[bit / 64] |= m;
	else
		buf[bit / 64] &= ~m;
}

/* Build this CPU's copies of RDR0_0, RDR4_0 and RDR16_1 for cfg. */
static int perf_mako_build(struct perf_mako_cpu *pc,
			   const struct perf_mako_config *cfg)
{
	bool core_b = perf_mako_core_b();
	bool filter = cfg->filter;
	uint32_t mask = cfg->mask, match = cfg->match;
	unsigned int j;

	if (cfg->set == MAKO_EVENTS_TLB_HANDLER && core_b)
		return -EOPNOTSUPP;

	memcpy(pc->rdr0_0, perf_mako_rdr0_0, sizeof(pc->rdr0_0));
	if (cfg->ctr2 == MAKO_CTR2_MEMOPS) {
		pc->rdr0_0[0] &= ~MAKO_RDR0_0_CLASS;
		pc->rdr0_0[0] |= FIELD_PREP(MAKO_RDR0_0_CLASS, MAKO_RDR0_0_MEMOPS);
	} else if (cfg->ctr2 == MAKO_CTR2_DMISSES ||
		   cfg->ctr2 == MAKO_CTR2_STORE_DMISSES) {
		pc->rdr0_0[0] &= ~MAKO_RDR0_0_CLASS;
		pc->rdr0_0[0] |= FIELD_PREP(MAKO_RDR0_0_CLASS, MAKO_RDR0_0_DMISSES);
	}
	if (cfg->ctr2 == MAKO_CTR2_STORE_DMISSES)
		perf_mako_set_bit(pc->rdr0_0, MAKO_RDR0_0_CTR2_STORES, true);
	if (cfg->ctr0 == MAKO_CTR0_STORES)
		perf_mako_set_bit(pc->rdr0_0, MAKO_RDR0_0_CTR0_STORES, true);

	memcpy(pc->rdr4_0, perf_mako_rdr4_0, sizeof(pc->rdr4_0));
	if (cfg->ctr2 == MAKO_CTR2_TAKEN) {
		pc->rdr4_0[0] &= ~GENMASK_ULL(63, 32);
		pc->rdr4_0[0] |= (uint64_t)MAKO_RDR4_0_TAKEN << 32;
	}
	if (cfg->ctr0 == MAKO_CTR0_DTLB)
		perf_mako_set_bit(pc->rdr4_0, MAKO_RDR4_0_CTR0_NO_ITLB, true);
	else if (cfg->ctr0 == MAKO_CTR0_ITLB)
		perf_mako_set_bit(pc->rdr4_0, MAKO_RDR4_0_CTR0_NO_DTLB, true);
	else if (cfg->ctr0 == MAKO_CTR0_INSNS || cfg->ctr0 == MAKO_CTR0_STORES)
		perf_mako_set_bit(pc->rdr4_0, MAKO_RDR4_0_CTR0_INSNS, true);
	if (cfg->ctr2 == MAKO_CTR2_BRANCHES) {
		filter = true;
		mask = match = 0x80000000;
		perf_mako_set_bit(pc->rdr4_0, MAKO_RDR4_0_BRANCH, false);
		for (j = 0; j < ARRAY_SIZE(perf_mako_rdr4_0_branch_set); j++)
			perf_mako_set_bit(pc->rdr4_0,
					  perf_mako_rdr4_0_branch_set[j], true);
	}
	if (filter) {
		for (j = 0; j < 32; j++) {
			uint32_t b = 1U << (31 - j);

			perf_mako_set_bit(pc->rdr4_0, j <= 18 ? 145 - j : 282 - j,
					  !(mask & b));
			perf_mako_set_bit(pc->rdr4_0, j <= 18 ? 108 + j : 205 + j,
					  match & b);
		}
		perf_mako_set_bit(pc->rdr4_0, MAKO_RDR4_0_INVERT,
				  cfg->filter && cfg->invert);
	}

	memcpy(pc->rdr16_1, perf_mako_rdr16_1, sizeof(pc->rdr16_1));
	if (cfg->set == MAKO_EVENTS_STALL) {
		perf_mako_set_bit(pc->rdr16_1, core_b ? MAKO_RDR16_1_STALL_B :
				  MAKO_RDR16_1_STALL_A, true);
	} else if (cfg->set == MAKO_EVENTS_NONSTALL && core_b) {
		perf_mako_set_bit(pc->rdr16_1, MAKO_RDR16_1_STALL_B, true);
		perf_mako_set_bit(pc->rdr16_1, MAKO_RDR16_1_NONSTALL_B, true);
		perf_mako_set_bit(pc->rdr16_1, MAKO_RDR16_1_NONSTALL_B_CTR2, true);
	} else if (cfg->set == MAKO_EVENTS_NONSTALL) {
		perf_mako_set_bit(pc->rdr16_1, MAKO_RDR16_1_NONSTALL, false);
	} else if (cfg->set == MAKO_EVENTS_TLB_HANDLER) {
		perf_mako_set_bit(pc->rdr16_1, MAKO_RDR16_1_TLB_HANDLER, true);
		perf_mako_set_bit(pc->rdr16_1, MAKO_RDR16_1_TLB_HANDLER_CLR, false);
	}

	return 0;
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
 * Write the image for cfg and start the counters on this CPU, from zero.
 * Called with interrupts off. Like the firmware, this holds chain 1 across
 * the image write and pmenb.
 */
int perf_mako_load(const struct perf_mako_config *cfg)
{
	struct perf_mako_cpu *pc = this_cpu_ptr(&perf_mako_cpu);
	unsigned long ccr;
	uint64_t ctrs[4];
	int i, ret;

	lockdep_assert_irqs_disabled();
	ret = perf_mako_build(pc, cfg);
	if (ret)
		return ret;
	if (pc->loaded)
		perf_intrigue_disable_perf_counters();
	pc->loaded = false;

	ccr = mfctl(10);
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
		else if (r->chain == 0 && r->rdr == 0)
			perf_mako_rdr_merge(r, pc->rdr0_0);
		else if (r->chain == 0 && r->rdr == 4)
			perf_mako_rdr_merge(r, pc->rdr4_0);
		else if (r->chain == 1 && r->rdr == 16)
			perf_mako_rdr_merge(r, pc->rdr16_1);
		else
			perf_mako_rdr_merge(r, r->data);
	}
	perf_mako_config_bit(false);

	asm volatile("pmenb" ::: "memory");
	WRITE_ONCE(*perf_mako_chip_key(), perf_mako_key(cfg));
	perf_mako_chain1_put();
	pc->cfg = *cfg;
	pc->loaded = true;
out:
	mtctl(ccr, 10);
	return ret;
}

/*
 * Read this CPU's four counters. Called with interrupts off and the
 * counters stopped.
 */
int perf_mako_read(uint32_t *c)
{
	unsigned long ccr;
	uint64_t buf[4];
	int i, w, ret;

	lockdep_assert_irqs_disabled();
	if (!this_cpu_read(perf_mako_cpu.loaded))
		return -EINVAL;

	ccr = mfctl(10);
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

/* Start or stop the counters of a loaded CPU. Called with interrupts off. */
void perf_mako_enable(void)
{
	if (this_cpu_read(perf_mako_cpu.loaded))
		perf_intrigue_enable_perf_counters();
}

void perf_mako_disable(void)
{
	if (this_cpu_read(perf_mako_cpu.loaded))
		perf_intrigue_disable_perf_counters();
}

static bool perf_mako_cfg_equal(const struct perf_mako_config *a,
				const struct perf_mako_config *b)
{
	return a->set == b->set && a->ctr0 == b->ctr0 && a->ctr2 == b->ctr2 &&
	       a->filter == b->filter &&
	       (!a->filter || (a->invert == b->invert &&
			       a->mask == b->mask && a->match == b->match));
}

/*
 * Whether cfg is loaded on this CPU and still in effect, so that the
 * counters can be restarted without loading it again.
 */
bool perf_mako_is_loaded(const struct perf_mako_config *cfg)
{
	struct perf_mako_cpu *pc = this_cpu_ptr(&perf_mako_cpu);

	return pc->loaded && perf_mako_cfg_equal(&pc->cfg, cfg) &&
	       READ_ONCE(*perf_mako_chip_key()) == perf_mako_key(cfg);
}

/* PA_PERF_ON: load the default configuration, or restart. */
int perf_mako_start(void)
{
	unsigned long flags;
	int ret = 0;

	local_irq_save(flags);
	if (perf_mako_is_loaded(&perf_mako_dev_cfg))
		perf_mako_enable();
	else
		ret = perf_mako_load(&perf_mako_dev_cfg);
	local_irq_restore(flags);

	return ret;
}

/* PA_PERF_OFF */
int perf_mako_stop(uint32_t *raddr)
{
	unsigned long flags;
	int ret;

	local_irq_save(flags);
	perf_mako_disable();
	ret = perf_mako_read(raddr);
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
	ret = perf_mako_load(&perf_mako_dev_cfg);
	local_irq_restore(flags);

	return ret ? ret : count;
}

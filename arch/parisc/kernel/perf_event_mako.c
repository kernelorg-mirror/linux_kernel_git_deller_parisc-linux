// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * perf events for the PA-8800/PA-8900 (Mako) performance counters
 *
 * Each core has four 31-bit counters, which count fixed events chosen by
 * an event set (see perf_mako.c):
 *
 *   ctr0: TLB misses, instruction and data, or only one kind, or
 *         instructions retired and not nullified
 *   ctr1, ctr3: cycles, stall cycles, non-stall cycles or, on the first
 *               core of a processor, cycles handling TLB misses
 *   ctr2: instructions retired, optionally filtered by instruction, or
 *         instructions retired in non-stall cycles
 *
 * The counters do not interrupt, so only counting is supported, and they
 * count in all privilege levels. A counter wraps from 0x7fffffff to 0 and
 * sets bit 31, which stays set. At four instructions a cycle ctr2 wraps
 * in under half a second, so a per-CPU hrtimer reads the counters every
 * 200 ms.
 *
 * The stall and non-stall qualifiers gate ctr0 as well, so the ctr0
 * events belong to the default set.
 *
 * All events on a CPU must agree on the event set. The stall, non-stall
 * and TLB handler sets also disturb the other core of the processor, so
 * they are only used while that core has no events, and while one core
 * uses them the other core can not add any. perf multiplexes the events
 * that do not fit.
 */

#define pr_fmt(fmt) "perf: " fmt

#include <linux/bitfield.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/perf_event.h>
#include <linux/spinlock.h>
#include <asm/processor.h>

#include "perf_mako.h"

#define MAKO_NUM_CTRS		4
#define MAKO_POLL_NS		(200 * NSEC_PER_MSEC)
#define MAKO_CTR_MASK		GENMASK(30, 0)

enum {
	MAKO_EV_CYCLES,
	MAKO_EV_INSTRUCTIONS,
	MAKO_EV_TLB_MISSES,
	MAKO_EV_STALL_CYCLES,
	MAKO_EV_NONSTALL_CYCLES,
	MAKO_EV_NONSTALL_INSTRUCTIONS,
	MAKO_EV_FILTERED_INSTRUCTIONS,
	MAKO_EV_DTLB_MISSES,
	MAKO_EV_ITLB_MISSES,
	MAKO_EV_NONNULLIFIED_INSTRUCTIONS,
	MAKO_EV_TLB_HANDLER_CYCLES,
	MAKO_EV_MAX,
};

#define MAKO_SET_ANY		-1

static const struct {
	u8	ctrs;		/* counters that can count it */
	s8	set;		/* MAKO_EVENTS_* */
} mako_events[MAKO_EV_MAX] = {
	[MAKO_EV_CYCLES]		= { BIT(1) | BIT(3), MAKO_EVENTS_DEFAULT },
	[MAKO_EV_INSTRUCTIONS]		= { BIT(2), MAKO_EVENTS_DEFAULT },
	[MAKO_EV_TLB_MISSES]		= { BIT(0), MAKO_EVENTS_DEFAULT },
	[MAKO_EV_STALL_CYCLES]		= { BIT(1) | BIT(3), MAKO_EVENTS_STALL },
	[MAKO_EV_NONSTALL_CYCLES]	= { BIT(1) | BIT(3), MAKO_EVENTS_NONSTALL },
	[MAKO_EV_NONSTALL_INSTRUCTIONS]	= { BIT(2), MAKO_EVENTS_NONSTALL },
	[MAKO_EV_FILTERED_INSTRUCTIONS]	= { BIT(2), MAKO_EVENTS_DEFAULT },
	[MAKO_EV_DTLB_MISSES]		= { BIT(0), MAKO_EVENTS_DEFAULT },
	[MAKO_EV_ITLB_MISSES]		= { BIT(0), MAKO_EVENTS_DEFAULT },
	[MAKO_EV_NONNULLIFIED_INSTRUCTIONS] = { BIT(0), MAKO_EVENTS_DEFAULT },
	[MAKO_EV_TLB_HANDLER_CYCLES]	= { BIT(1) | BIT(3), MAKO_EVENTS_TLB_HANDLER },
};

#define MAKO_CONFIG_EVENT	GENMASK_ULL(7, 0)
#define MAKO_CONFIG_INVERT	BIT_ULL(8)
#define MAKO_CONFIG1_MASK	GENMASK_ULL(31, 0)
#define MAKO_CONFIG1_MATCH	GENMASK_ULL(63, 32)

struct mako_pmu_cpu {
	struct perf_event	*events[MAKO_NUM_CTRS];
	u32			ctrs[MAKO_NUM_CTRS];	/* last read */
	bool			running;		/* counters enabled */
	struct hrtimer		timer;
};

static DEFINE_PER_CPU(struct mako_pmu_cpu, mako_pmu_cpu);

/*
 * The event set each core of a processor uses, MAKO_SET_ANY when it has no
 * events. Kept at perf_mako_chip_cpu() and indexed by perf_mako_core_b().
 */
struct mako_chip {
	raw_spinlock_t	lock;
	int		set[2];
};

static DEFINE_PER_CPU(struct mako_chip, mako_chip);

static struct mako_chip *mako_this_chip(void)
{
	return per_cpu_ptr(&mako_chip, perf_mako_chip_cpu(smp_processor_id()));
}

/*
 * The event code, after event_init(), which leaves it and the invert flag in
 * hw.config.
 */
static unsigned int mako_event_code(struct perf_event *event)
{
	return FIELD_GET(MAKO_CONFIG_EVENT, event->hw.config);
}

/*
 * Assign counters to the events, and work out the event set and filter
 * they need. Returns false if they do not fit together on one core.
 */
static bool mako_schedule(struct perf_event **events, int n, int *idx,
			  struct perf_mako_config *cfg)
{
	u8 used = 0;
	int i, set = MAKO_SET_ANY;

	memset(cfg, 0, sizeof(*cfg));
	for (i = 0; i < n; i++) {
		unsigned int code = mako_event_code(events[i]);
		u8 free = mako_events[code].ctrs & ~used;
		int s = mako_events[code].set;

		if (!free)
			return false;
		if (set != MAKO_SET_ANY && set != s)
			return false;
		set = s;
		idx[i] = __ffs(free);
		used |= BIT(idx[i]);

		if (code == MAKO_EV_DTLB_MISSES)
			cfg->ctr0 = MAKO_CTR0_DTLB;
		else if (code == MAKO_EV_ITLB_MISSES)
			cfg->ctr0 = MAKO_CTR0_ITLB;
		else if (code == MAKO_EV_NONNULLIFIED_INSTRUCTIONS)
			cfg->ctr0 = MAKO_CTR0_INSNS;

		if (code == MAKO_EV_FILTERED_INSTRUCTIONS) {
			u64 c1 = events[i]->attr.config1;

			cfg->filter = true;
			cfg->invert = events[i]->hw.config & MAKO_CONFIG_INVERT;
			cfg->mask = FIELD_GET(MAKO_CONFIG1_MASK, c1);
			cfg->match = FIELD_GET(MAKO_CONFIG1_MATCH, c1);
		}
	}
	cfg->set = set == MAKO_SET_ANY ? MAKO_EVENTS_DEFAULT : set;

	return true;
}

/* Gather the events on this CPU, plus extra if not NULL. */
static int mako_collect(struct mako_pmu_cpu *pc, struct perf_event *extra,
			struct perf_event **events)
{
	int i, n = 0;

	for (i = 0; i < MAKO_NUM_CTRS; i++)
		if (pc->events[i])
			events[n++] = pc->events[i];
	if (extra)
		events[n++] = extra;

	return n;
}

static void mako_event_update(struct perf_event *event, u32 now)
{
	struct hw_perf_event *hwc = &event->hw;
	u32 prev = local64_read(&hwc->prev_count);

	local64_set(&hwc->prev_count, now);
	local64_add((now - prev) & MAKO_CTR_MASK, &event->count);
}

/*
 * Read the counters, and if they are running for perf, update every
 * started event. The counters have to be stopped to be read. They may
 * have been left running by /dev/perf, so this always reads them. Called
 * with interrupts off.
 */
static void mako_read_all(struct mako_pmu_cpu *pc)
{
	int i;

	perf_mako_disable();
	if (perf_mako_read(pc->ctrs) == 0 && pc->running) {
		for (i = 0; i < MAKO_NUM_CTRS; i++) {
			struct perf_event *event = pc->events[i];

			if (event && !(event->hw.state & PERF_HES_STOPPED))
				mako_event_update(event, pc->ctrs[i]);
		}
	}
	if (pc->running)
		perf_mako_enable();
}

static enum hrtimer_restart mako_poll(struct hrtimer *timer)
{
	struct mako_pmu_cpu *pc = container_of(timer, struct mako_pmu_cpu, timer);

	mako_read_all(pc);
	hrtimer_forward_now(timer, ns_to_ktime(MAKO_POLL_NS));

	return HRTIMER_RESTART;
}

static void mako_pmu_disable(struct pmu *pmu)
{
	struct mako_pmu_cpu *pc = this_cpu_ptr(&mako_pmu_cpu);

	mako_read_all(pc);
	perf_mako_disable();
	pc->running = false;
}

static void mako_pmu_enable(struct pmu *pmu)
{
	struct mako_pmu_cpu *pc = this_cpu_ptr(&mako_pmu_cpu);
	struct perf_event *events[MAKO_NUM_CTRS];
	struct perf_mako_config cfg;
	int idx[MAKO_NUM_CTRS];
	int i, n;

	if (pc->running)
		return;

	n = mako_collect(pc, NULL, events);
	if (!n) {
		hrtimer_try_to_cancel(&pc->timer);
		return;
	}

	/* add() placed the events, this only recomputes the set. */
	if (WARN_ON_ONCE(!mako_schedule(events, n, idx, &cfg)))
		return;

	if (perf_mako_is_loaded(&cfg)) {
		perf_mako_enable();
	} else {
		int ret = perf_mako_load(&cfg);

		if (ret) {
			pr_warn_once("cannot load the counter image: %d\n", ret);
			return;
		}
		memset(pc->ctrs, 0, sizeof(pc->ctrs));
		for (i = 0; i < n; i++)
			local64_set(&events[i]->hw.prev_count, 0);
	}
	pc->running = true;

	if (!hrtimer_active(&pc->timer))
		hrtimer_start(&pc->timer, ns_to_ktime(MAKO_POLL_NS),
			      HRTIMER_MODE_REL_PINNED_HARD);
}

static void mako_pmu_start(struct perf_event *event, int flags)
{
	struct mako_pmu_cpu *pc = this_cpu_ptr(&mako_pmu_cpu);
	struct hw_perf_event *hwc = &event->hw;

	mako_read_all(pc);
	local64_set(&hwc->prev_count, pc->ctrs[hwc->idx]);
	hwc->state = 0;
}

static void mako_pmu_stop(struct perf_event *event, int flags)
{
	struct mako_pmu_cpu *pc = this_cpu_ptr(&mako_pmu_cpu);
	struct hw_perf_event *hwc = &event->hw;

	if (hwc->state & PERF_HES_STOPPED)
		return;

	mako_read_all(pc);
	hwc->state |= PERF_HES_STOPPED | PERF_HES_UPTODATE;
}

/* Record the event set this core needs, if the other core allows it. */
static bool mako_chip_claim(int set)
{
	struct mako_chip *chip = mako_this_chip();
	int core = perf_mako_core_b(), other;
	bool ok;

	raw_spin_lock(&chip->lock);
	other = chip->set[!core];
	ok = other == MAKO_SET_ANY ||
	     (other == MAKO_EVENTS_DEFAULT && set == MAKO_EVENTS_DEFAULT);
	if (ok)
		chip->set[core] = set;
	raw_spin_unlock(&chip->lock);

	return ok;
}

static int mako_pmu_add(struct perf_event *event, int flags)
{
	struct mako_pmu_cpu *pc = this_cpu_ptr(&mako_pmu_cpu);
	struct perf_event *events[MAKO_NUM_CTRS + 1];
	unsigned int code = mako_event_code(event);
	struct perf_mako_config cfg;
	int idx[MAKO_NUM_CTRS + 1];
	unsigned long free = 0;
	int i, n;

	n = mako_collect(pc, event, events);
	if (n > MAKO_NUM_CTRS || !mako_schedule(events, n, idx, &cfg))
		return -EAGAIN;
	/* A task's events can move to the second core, which cannot count it. */
	if (cfg.set == MAKO_EVENTS_TLB_HANDLER && perf_mako_core_b())
		return -EAGAIN;
	if (!mako_chip_claim(cfg.set))
		return -EAGAIN;

	/*
	 * Keep the events already here on their counters. Each event can
	 * use either only one counter or only ctr1 and ctr3, so if the
	 * events fit together, one of this event's counters is free.
	 */
	for (i = 0; i < MAKO_NUM_CTRS; i++)
		if (!pc->events[i] && (mako_events[code].ctrs & BIT(i)))
			free |= BIT(i);

	event->hw.idx = __ffs(free);
	pc->events[event->hw.idx] = event;

	event->hw.state = PERF_HES_STOPPED | PERF_HES_UPTODATE;
	if (flags & PERF_EF_START)
		mako_pmu_start(event, PERF_EF_RELOAD);

	return 0;
}

static void mako_pmu_del(struct perf_event *event, int flags)
{
	struct mako_pmu_cpu *pc = this_cpu_ptr(&mako_pmu_cpu);
	struct mako_chip *chip = mako_this_chip();
	struct perf_event *events[MAKO_NUM_CTRS];
	int core = perf_mako_core_b();
	struct perf_mako_config cfg;
	int idx[MAKO_NUM_CTRS];
	int n;

	mako_pmu_stop(event, PERF_EF_UPDATE);
	pc->events[event->hw.idx] = NULL;
	perf_event_update_userpage(event);

	n = mako_collect(pc, NULL, events);
	raw_spin_lock(&chip->lock);
	if (!n)
		chip->set[core] = MAKO_SET_ANY;
	else if (mako_schedule(events, n, idx, &cfg))
		chip->set[core] = cfg.set;
	raw_spin_unlock(&chip->lock);
}

static void mako_pmu_read(struct perf_event *event)
{
	mako_read_all(this_cpu_ptr(&mako_pmu_cpu));
}

static int mako_map_hw(u64 config)
{
	switch (config) {
	case PERF_COUNT_HW_CPU_CYCLES:
		return MAKO_EV_CYCLES;
	case PERF_COUNT_HW_INSTRUCTIONS:
		return MAKO_EV_INSTRUCTIONS;
	default:
		return -ENOENT;
	}
}

static void mako_event_destroy(struct perf_event *event)
{
	perf_mako_pmu_put();
}

static int mako_pmu_event_init(struct perf_event *event)
{
	struct perf_event *leader = event->group_leader, *sibling;
	struct perf_event *events[MAKO_NUM_CTRS + 1];
	struct perf_mako_config cfg;
	int idx[MAKO_NUM_CTRS + 1];
	int code, n = 0;
	u64 config;

	switch (event->attr.type) {
	case PERF_TYPE_HARDWARE:
		code = mako_map_hw(event->attr.config);
		break;
	case PERF_TYPE_RAW:
		code = FIELD_GET(MAKO_CONFIG_EVENT, event->attr.config);
		break;
	default:
		return -ENOENT;
	}
	if (code < 0)
		return code;
	if (code >= MAKO_EV_MAX)
		return -EINVAL;

	/* The filter fields are only valid on filtered instructions. */
	config = event->attr.type == PERF_TYPE_RAW ? event->attr.config : code;
	if (code != MAKO_EV_FILTERED_INSTRUCTIONS &&
	    ((config & MAKO_CONFIG_INVERT) || event->attr.config1))
		return -EINVAL;
	if (config & ~(MAKO_CONFIG_EVENT | MAKO_CONFIG_INVERT))
		return -EINVAL;

	event->hw.config = code | (config & MAKO_CONFIG_INVERT);

	/* The whole group has to fit on one core. */
	if (leader != event && leader->pmu == event->pmu)
		events[n++] = leader;
	for_each_sibling_event(sibling, leader) {
		if (sibling->pmu != event->pmu)
			continue;
		if (n >= MAKO_NUM_CTRS)
			return -EINVAL;
		events[n++] = sibling;
	}
	events[n++] = event;
	if (!mako_schedule(events, n, idx, &cfg))
		return -EINVAL;

	if (!perf_mako_pmu_get())
		return -EBUSY;
	event->destroy = mako_event_destroy;
	event->hw.idx = -1;

	return 0;
}

PMU_FORMAT_ATTR(event, "config:0-7");
PMU_FORMAT_ATTR(invert, "config:8");
PMU_FORMAT_ATTR(mask, "config1:0-31");
PMU_FORMAT_ATTR(match, "config1:32-63");

static struct attribute *mako_format_attrs[] = {
	&format_attr_event.attr,
	&format_attr_invert.attr,
	&format_attr_mask.attr,
	&format_attr_match.attr,
	NULL,
};

static const struct attribute_group mako_format_group = {
	.name = "format",
	.attrs = mako_format_attrs,
};

#define MAKO_EVENT_ATTR(_name, _code) \
	PMU_EVENT_ATTR_STRING(_name, mako_attr_##_code, "event=" __stringify(_code))

MAKO_EVENT_ATTR(cycles, 0);
MAKO_EVENT_ATTR(instructions, 1);
MAKO_EVENT_ATTR(tlb-misses, 2);
MAKO_EVENT_ATTR(stall-cycles, 3);
MAKO_EVENT_ATTR(nonstall-cycles, 4);
MAKO_EVENT_ATTR(nonstall-instructions, 5);
MAKO_EVENT_ATTR(dtlb-misses, 7);
MAKO_EVENT_ATTR(itlb-misses, 8);
MAKO_EVENT_ATTR(nonnullified-instructions, 9);
MAKO_EVENT_ATTR(tlb-handler-cycles, 10);

static struct attribute *mako_event_attrs[] = {
	&mako_attr_0.attr.attr,
	&mako_attr_1.attr.attr,
	&mako_attr_2.attr.attr,
	&mako_attr_3.attr.attr,
	&mako_attr_4.attr.attr,
	&mako_attr_5.attr.attr,
	&mako_attr_7.attr.attr,
	&mako_attr_8.attr.attr,
	&mako_attr_9.attr.attr,
	&mako_attr_10.attr.attr,
	NULL,
};

static const struct attribute_group mako_events_group = {
	.name = "events",
	.attrs = mako_event_attrs,
};

static const struct attribute_group *mako_attr_groups[] = {
	&mako_format_group,
	&mako_events_group,
	NULL,
};

static struct pmu mako_pmu = {
	.task_ctx_nr	= perf_hw_context,
	.capabilities	= PERF_PMU_CAP_NO_INTERRUPT | PERF_PMU_CAP_NO_EXCLUDE,
	.attr_groups	= mako_attr_groups,
	.event_init	= mako_pmu_event_init,
	.pmu_enable	= mako_pmu_enable,
	.pmu_disable	= mako_pmu_disable,
	.add		= mako_pmu_add,
	.del		= mako_pmu_del,
	.start		= mako_pmu_start,
	.stop		= mako_pmu_stop,
	.read		= mako_pmu_read,
};

static int __init mako_pmu_init(void)
{
	int cpu;

	if (boot_cpu_data.cpu_type != mako && boot_cpu_data.cpu_type != mako2)
		return 0;

	for_each_possible_cpu(cpu) {
		struct mako_chip *chip = per_cpu_ptr(&mako_chip, cpu);

		raw_spin_lock_init(&chip->lock);
		chip->set[0] = MAKO_SET_ANY;
		chip->set[1] = MAKO_SET_ANY;
		hrtimer_setup(&per_cpu(mako_pmu_cpu, cpu).timer, mako_poll,
			      CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	}

	return perf_pmu_register(&mako_pmu, "cpu", PERF_TYPE_RAW);
}
device_initcall(mako_pmu_init);

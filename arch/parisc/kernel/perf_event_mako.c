// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * perf events for the PA-8800/PA-8900 (Mako) performance counters
 *
 * Each core has four 31-bit counters (see perf_mako.c):
 *
 *   ctr0: TLB misses, instruction and data, or only one kind, or
 *         instructions retired and not nullified
 *   ctr1, ctr3: cycles
 *   ctr2: instructions retired
 *
 * The counters do not interrupt, so only counting is supported, and they
 * count in all privilege levels. A counter wraps from 0x7fffffff to 0 and
 * sets bit 31, which stays set. At four instructions a cycle ctr2 wraps
 * in under half a second, so a per-CPU hrtimer reads the counters every
 * 200 ms.
 */

#define pr_fmt(fmt) "perf: " fmt

#include <linux/bits.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/perf_event.h>
#include <asm/processor.h>

#include "perf_mako.h"

#define MAKO_NUM_CTRS		4
#define MAKO_POLL_NS		(200 * NSEC_PER_MSEC)
#define MAKO_CTR_MASK		GENMASK(30, 0)

enum {
	MAKO_EV_CYCLES,
	MAKO_EV_INSTRUCTIONS,
	MAKO_EV_TLB_MISSES,
	MAKO_EV_DTLB_MISSES = 7,
	MAKO_EV_ITLB_MISSES,
	MAKO_EV_NONNULLIFIED_INSTRUCTIONS,
	MAKO_EV_MAX,
};

/* The counters that can count each event, none for unused codes. */
static const u8 mako_ctrs[MAKO_EV_MAX] = {
	[MAKO_EV_CYCLES]		= BIT(1) | BIT(3),
	[MAKO_EV_INSTRUCTIONS]		= BIT(2),
	[MAKO_EV_TLB_MISSES]		= BIT(0),
	[MAKO_EV_DTLB_MISSES]		= BIT(0),
	[MAKO_EV_ITLB_MISSES]		= BIT(0),
	[MAKO_EV_NONNULLIFIED_INSTRUCTIONS] = BIT(0),
};

#define MAKO_CONFIG_EVENT	GENMASK_ULL(7, 0)

struct mako_pmu_cpu {
	struct perf_event	*events[MAKO_NUM_CTRS];
	u32			ctrs[MAKO_NUM_CTRS];	/* last read */
	bool			running;		/* counters enabled */
	struct hrtimer		timer;
};

static DEFINE_PER_CPU(struct mako_pmu_cpu, mako_pmu_cpu);

/* The event code, which event_init() leaves in hw.config. */
static unsigned int mako_event_code(struct perf_event *event)
{
	return event->hw.config;
}

/*
 * Assign counters to the events, and work out the configuration they
 * need. Returns false if they do not fit together on one core.
 */
static bool mako_schedule(struct perf_event **events, int n, int *idx,
			  struct perf_mako_config *cfg)
{
	u8 used = 0;
	int i;

	memset(cfg, 0, sizeof(*cfg));
	for (i = 0; i < n; i++) {
		unsigned int code = mako_event_code(events[i]);
		u8 free = mako_ctrs[code] & ~used;

		if (!free)
			return false;
		idx[i] = __ffs(free);
		used |= BIT(idx[i]);

		if (code == MAKO_EV_DTLB_MISSES)
			cfg->ctr0 = MAKO_CTR0_DTLB;
		else if (code == MAKO_EV_ITLB_MISSES)
			cfg->ctr0 = MAKO_CTR0_ITLB;
		else if (code == MAKO_EV_NONNULLIFIED_INSTRUCTIONS)
			cfg->ctr0 = MAKO_CTR0_INSNS;
	}

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

	/* add() placed the events, this only recomputes the configuration. */
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

	/*
	 * Keep the events already here on their counters. Each event can
	 * use either only one counter or only ctr1 and ctr3, so if the
	 * events fit together, one of this event's counters is free.
	 */
	for (i = 0; i < MAKO_NUM_CTRS; i++)
		if (!pc->events[i] && (mako_ctrs[code] & BIT(i)))
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

	mako_pmu_stop(event, PERF_EF_UPDATE);
	pc->events[event->hw.idx] = NULL;
	perf_event_update_userpage(event);
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

	switch (event->attr.type) {
	case PERF_TYPE_HARDWARE:
		code = mako_map_hw(event->attr.config);
		break;
	case PERF_TYPE_RAW:
		if (event->attr.config & ~MAKO_CONFIG_EVENT)
			return -EINVAL;
		code = event->attr.config;
		break;
	default:
		return -ENOENT;
	}
	if (code < 0)
		return code;
	if (code >= MAKO_EV_MAX || !mako_ctrs[code])
		return -EINVAL;

	event->hw.config = code;

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

static struct attribute *mako_format_attrs[] = {
	&format_attr_event.attr,
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
MAKO_EVENT_ATTR(dtlb-misses, 7);
MAKO_EVENT_ATTR(itlb-misses, 8);
MAKO_EVENT_ATTR(nonnullified-instructions, 9);

static struct attribute *mako_event_attrs[] = {
	&mako_attr_0.attr.attr,
	&mako_attr_1.attr.attr,
	&mako_attr_2.attr.attr,
	&mako_attr_7.attr.attr,
	&mako_attr_8.attr.attr,
	&mako_attr_9.attr.attr,
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

	for_each_possible_cpu(cpu)
		hrtimer_setup(&per_cpu(mako_pmu_cpu, cpu).timer, mako_poll,
			      CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);

	return perf_pmu_register(&mako_pmu, "cpu", PERF_TYPE_RAW);
}
device_initcall(mako_pmu_init);

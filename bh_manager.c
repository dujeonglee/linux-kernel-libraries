// SPDX-License-Identifier: GPL-2.0
/******************************************************************************
 *
 * Copyright (C) 2026 Dujeong Lee <dujeong.lee82@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * bh_manager — unified BH / tput / cpuhp manager (implementation)
 *
 * Self-contained: no dependency on slsi_dev, netdev_vif, or any driver
 * internal type. Netdevs participating in aggregate throughput measurement
 * are tracked via an explicit register/unregister API.
 *
 *****************************************************************************/

#define pr_fmt(fmt) "bhm: " fmt

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/err.h>
#include <linux/etherdevice.h>
#include <linux/if_link.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <linux/sched.h>
#include <linux/sched/topology.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "bh_manager.h"

#define BHM_TIMER_PERIOD_MS          100
#define BHM_DEFAULT_RISE_TICKS       1
#define BHM_DEFAULT_FALL_TICKS       10
#define BHM_INVALID_LEVEL            ((u32)-1)

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

enum bhm_evt_kind {
	BHM_EVT_NONE             = 0,
	BHM_EVT_LEVEL_TRANSITION = (1 << 0),
	BHM_EVT_PERIODIC_TICK    = (1 << 1),
	BHM_EVT_OVERRIDE_EDGE    = (1 << 2),
};

/* Per-CPU IPI slot used by NAPI and TASKLET preferred-CPU dispatch. */
struct bhm_per_cpu {
	call_single_data_t  csd;
	unsigned long       queued;   /* 1-bit; test_and_set_bit(0, &queued) */
	struct bhm_bh     *bh;
};

struct bhm_netdev {
	struct list_head   node;       /* g_mgr.netdev_list */
	struct net_device *dev;
	u64                last_tx_bytes;
	u64                last_rx_bytes;
	ktime_t            last_sample;
	bool               primed;     /* baseline captured → deltas valid */

	/* Per-tick throughput snapshot (bits/sec), refreshed by
	 * bhm_sample_tput_locked each 100ms. Read by BHs under g_mgr.lock
	 * to compute their filtered aggregate.
	 */
	u32                tx_bps;
	u32                rx_bps;
};

struct bhm_bh {
	struct list_head         node;     /* g_mgr.bh_list */
	void                    *priv;

	enum bhm_type        type;

	/* Config copies (registration-time, immutable) */
	struct bhm_level    *levels;
	u32                      nr_levels;
	bool                     has_periodic;   /* levels[0] is PERIODIC */
	struct bhm_hysteresis   hyst;
	struct bhm_override_cfg ovcfg;

	/* Optional netdev name filter: NULL-terminated list of interface
	 * names whose throughput contributes to this BH's aggregate. NULL
	 * means "all registered netdevs" (global aggregate). Pointer and
	 * pointees are caller-owned and must outlive the BH.
	 */
	const char * const      *netdev_names;

	/* Preferred-CPU dispatch.
	 *   per_cpu:        dynamic per-CPU IPI slots (alloc_percpu).
	 *                   Allocated only for NAPI and TASKLET — the types
	 *                   that use IPI-based cross-CPU dispatch. NULL for
	 *                   THREADED_NAPI / WORKQUEUE.
	 *   preferred_mask: user-supplied allowed set. Empty = no preference.
	 *                   Writes under g_mgr.lock; hot-path reads unlocked
	 *                   (torn reads are benign — mask is a placement hint,
	 *                   mispicks self-correct on the next dispatch).
	 *
	 * Placement policy (see bhm_resolve_preferred_cpu):
	 *   1. If the current CPU is inside preferred_mask, stay — honors
	 *      whatever delivered the IRQ/event here (IRQ affinity, softirq
	 *      locality). Also warms cache.
	 *   2. Else pick the HIGHEST-CAPACITY idle CPU within the mask
	 *      (big.LITTLE aware via arch_scale_cpu_capacity).
	 *   3. Else (nobody idle) fair-distribute via
	 *      cpumask_any_and_distribute.
	 */
	struct bhm_per_cpu __percpu *per_cpu;
	cpumask_t                        preferred_mask;

	/* State machine (protected by g_mgr.lock) */
	u32                      current_level;
	u32                      candidate_level;
	u32                      candidate_ticks;
	bool                     override_on;
	u32                      sat_streak;

	/* Override auto-clear */
	struct delayed_work      override_clear_work;

	/* Dispatch (sleepable callback execution) */
	struct work_struct       dispatch_work;
	u32                      pending_evt;        /* bitmask of BHM_EVT_* */
	u32                      pending_level;
	u32                      pending_tput_tx;
	u32                      pending_tput_rx;
	bool                     pending_override;

	/* Net-change filter for edge events. Accessed only from
	 * bhm_dispatch_work_fn, which the workqueue serializes per work
	 * item — no lock needed. Suppresses edge callbacks whose delivered
	 * state matches what the consumer was last told, so bursts that net
	 * to no change (e.g. off→on→off within one dispatch cycle) collapse
	 * into a single net notification. Periodic ticks bypass this filter.
	 */
	u32                      last_reported_level;
	bool                     last_reported_override;

	/* BH backend state */
	union {
		struct {
			struct napi_struct   napi;
			struct net_device   *dev;      /* may be &g_mgr.dummy_dev */
			int                (*user_poll)(struct napi_struct *, int);
			int                  weight;
			bool                 threaded;
			bool                 added;
		} napi;
		struct {
			struct tasklet_struct  t;
			void                 (*user_fn)(unsigned long);
			unsigned long          data;
		} tasklet;
		struct {
			struct work_struct       w;
			struct workqueue_struct *wq;
			void                   (*user_fn)(struct work_struct *);
		} work;
	} be;
};

struct bhm_mgr {
	spinlock_t        lock;            /* guards bh_list + bh state + netdev_list */
	struct list_head  bh_list;
	struct list_head  netdev_list;     /* bhm_netdev entries */

	struct timer_list timer;
	bool              timer_armed;

	struct workqueue_struct *wq;

	cpumask_t         cpu_avail;
	spinlock_t        cpu_lock;
	enum cpuhp_state  cpuhp_state;
	bool              cpuhp_registered;

	struct net_device dummy_dev;       /* NAPI anchor when caller supplies none */

	u32               last_tput_tx;
	u32               last_tput_rx;

	bool              initialized;
};

static struct bhm_mgr g_mgr;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static inline bool bhm_level_is_periodic(const struct bhm_level *lv)
{
	return lv->threshold_bps == BHM_LEVEL_PERIODIC;
}

static inline u32 bhm_tput_for_dir(u32 tx, u32 rx, enum bhm_tput_dir d)
{
	switch (d) {
	case BHM_DIR_TX:   return tx;
	case BHM_DIR_RX:   return rx;
	case BHM_DIR_BOTH: /* fallthrough */
	default:                 return tx + rx;
	}
}

/* Select active level for (tx, rx). Walk high→low; first satisfying level
 * wins. If none satisfies, return index 0 (PERIODIC floor or threshold==0).
 */
static u32 bhm_select_level(const struct bhm_bh *bh, u32 tx, u32 rx)
{
	int i;

	for (i = (int)bh->nr_levels - 1; i >= 0; i--) {
		const struct bhm_level *lv = &bh->levels[i];
		u32 v;

		if (bhm_level_is_periodic(lv))
			continue;
		v = bhm_tput_for_dir(tx, rx, lv->dir);
		if (v >= lv->threshold_bps)
			return (u32)i;
	}
	return 0;
}

static void bhm_schedule_dispatch(struct bhm_bh *bh)
{
	if (g_mgr.wq)
		queue_work(g_mgr.wq, &bh->dispatch_work);
}

static void bhm_latch_event(struct bhm_bh *bh, u32 evt_bit, u32 level,
			   u32 tx, u32 rx, bool override_state)
{
	bh->pending_evt      |= evt_bit;
	bh->pending_level     = level;
	bh->pending_tput_tx   = tx;
	bh->pending_tput_rx   = rx;
	bh->pending_override  = override_state;
}

/* -------------------------------------------------------------------------
 * Dispatcher — runs in workqueue, invokes the user callback
 * ---------------------------------------------------------------------- */

static void bhm_dispatch_work_fn(struct work_struct *w)
{
	struct bhm_bh *bh = container_of(w, struct bhm_bh, dispatch_work);
	u32 evt, level, tx, rx;
	bool override_state;
	cpumask_t avail_snapshot;
	bhm_level_cb cb;
	void *ctx;
	unsigned long flags;

	spin_lock_irqsave(&g_mgr.lock, flags);
	evt             = bh->pending_evt;
	level           = bh->pending_level;
	tx              = bh->pending_tput_tx;
	rx              = bh->pending_tput_rx;
	override_state  = bh->pending_override;
	bh->pending_evt = BHM_EVT_NONE;
	spin_unlock_irqrestore(&g_mgr.lock, flags);

	if (!evt || level >= bh->nr_levels)
		return;

	/* Net-change filter: drop edge events whose state already matches
	 * what the consumer was last told. Periodic ticks are not edges and
	 * pass through unchanged. Commit the new reported state before the
	 * callback runs so that a cb-initiated re-latch observes the value
	 * we're about to deliver.
	 */
	if ((evt & BHM_EVT_LEVEL_TRANSITION) &&
	    level == bh->last_reported_level)
		evt &= ~BHM_EVT_LEVEL_TRANSITION;

	if ((evt & BHM_EVT_OVERRIDE_EDGE) &&
	    override_state == bh->last_reported_override)
		evt &= ~BHM_EVT_OVERRIDE_EDGE;

	if (!evt)
		return;

	if (evt & BHM_EVT_LEVEL_TRANSITION)
		bh->last_reported_level = level;
	if (evt & BHM_EVT_OVERRIDE_EDGE)
		bh->last_reported_override = override_state;

	cb  = bh->levels[level].cb;
	ctx = bh->levels[level].ctx;
	if (!cb)
		return;

	spin_lock_irqsave(&g_mgr.cpu_lock, flags);
	cpumask_copy(&avail_snapshot, &g_mgr.cpu_avail);
	spin_unlock_irqrestore(&g_mgr.cpu_lock, flags);

	cb(bh, level, tx, rx, override_state, &avail_snapshot, ctx);
}

/* -------------------------------------------------------------------------
 * Override auto-clear work
 * ---------------------------------------------------------------------- */

static void bhm_override_clear_work_fn(struct work_struct *w)
{
	struct delayed_work *dw = to_delayed_work(w);
	struct bhm_bh *bh = container_of(dw, struct bhm_bh, override_clear_work);
	unsigned long flags;
	bool fire = false;
	u32 tx, rx, level;

	spin_lock_irqsave(&g_mgr.lock, flags);
	if (bh->override_on) {
		bh->override_on = false;
		bh->sat_streak  = 0;
		bhm_bh_tput_locked(bh, &tx, &rx);
		level = bh->current_level;
		bhm_latch_event(bh, BHM_EVT_OVERRIDE_EDGE, level, tx, rx, false);
		fire = true;
	}
	spin_unlock_irqrestore(&g_mgr.lock, flags);

	if (fire)
		bhm_schedule_dispatch(bh);
}

static void bhm_trigger_override_locked(struct bhm_bh *bh)
{
	u32 tx, rx;

	if (bh->override_on)
		return;

	bh->override_on = true;
	bhm_bh_tput_locked(bh, &tx, &rx);
	bhm_latch_event(bh, BHM_EVT_OVERRIDE_EDGE, bh->current_level, tx, rx, true);
	bhm_schedule_dispatch(bh);

	if (bh->ovcfg.timeout_ms)
		mod_delayed_work(g_mgr.wq, &bh->override_clear_work,
				 msecs_to_jiffies(bh->ovcfg.timeout_ms));
}

/* -------------------------------------------------------------------------
 * Saturation streak accounting (called from BH wrapper)
 * ---------------------------------------------------------------------- */

static void bhm_account_saturated(struct bhm_bh *bh, bool saturated)
{
	unsigned long flags;

	if (!bh->ovcfg.budget_full_streak)
		return;

	spin_lock_irqsave(&g_mgr.lock, flags);
	if (saturated) {
		bh->sat_streak++;
		if (bh->sat_streak >= bh->ovcfg.budget_full_streak)
			bhm_trigger_override_locked(bh);
	} else {
		bh->sat_streak = 0;
	}
	spin_unlock_irqrestore(&g_mgr.lock, flags);
}

/* -------------------------------------------------------------------------
 * Preferred-CPU IPI dispatch (NAPI + TASKLET)
 *
 * Each BH that participates owns a per-CPU slot allocated via alloc_percpu.
 * The slot's CSD, when fired on the target CPU, runs napi_schedule() or
 * tasklet_schedule() from that CPU, queuing the BH onto that CPU's softirq
 * list. A 1-bit `queued` flag per slot suppresses redundant IPIs and is
 * cleared by the CSD after dispatch — teardown busy-waits on it to avoid
 * UAF on bh pointer.
 * ---------------------------------------------------------------------- */

static void bhm_csd_fn(void *info)
{
	struct bhm_per_cpu *pc = info;
	struct bhm_bh *bh = pc->bh;

	switch (bh->type) {
	case BHM_TYPE_NAPI:
		napi_schedule(&bh->be.napi.napi);
		break;
	case BHM_TYPE_TASKLET:
		tasklet_schedule(&bh->be.tasklet.t);
		break;
	default:
		break;
	}
	/* Must be LAST: teardown waits on this bit to know it's safe to free
	 * the enclosing bh. After this store, we touch nothing of bh/pc.
	 */
	smp_mb__before_atomic();
	clear_bit(0, &pc->queued);
}

/* Decides whether we should redirect the BH to a different CPU.
 * On true, writes the target CPU to *preferred.
 */
static bool bhm_should_use_ipi(struct bhm_bh *bh, int *preferred)
{
	int picked;

	if (!bh->per_cpu)
		return false;

	picked = bhm_resolve_preferred_cpu(bh);
	if (picked < 0 || picked == smp_processor_id())
		return false;

	*preferred = picked;
	return true;
}

/* Queues an IPI to @preferred CPU. Returns 0 on success (including when the
 * slot already had a pending IPI), negative on dispatch failure.
 */
static int bhm_ipi_send(struct bhm_bh *bh, int preferred)
{
	struct bhm_per_cpu *pc = per_cpu_ptr(bh->per_cpu, preferred);
	int r;

	if (test_and_set_bit(0, &pc->queued))
		return 0;   /* already pending; target CPU will dispatch */

	r = smp_call_function_single_async(preferred, &pc->csd);
	if (r != 0) {
		clear_bit(0, &pc->queued);
		return r;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * NAPI / tasklet / workqueue wrappers
 * ---------------------------------------------------------------------- */

static int bhm_napi_poll_wrapper(struct napi_struct *napi, int budget)
{
	struct bhm_bh *bh = container_of(napi, struct bhm_bh, be.napi.napi);
	int preferred;
	int work_done;

	if (bhm_should_use_ipi(bh, &preferred)) {
		/* Order matters:
		 *   napi_complete_done MUST run BEFORE we issue the IPI so
		 *   the target CPU's napi_schedule() sees a cleared SCHED
		 *   bit. If we issued the IPI first and the CSD fired on
		 *   the target CPU while our NAPI was still in SCHED state,
		 *   napi_schedule would only set MISSED, and our subsequent
		 *   napi_complete_done would re-kick the NAPI *locally* —
		 *   defeating the migration.
		 */
		napi_complete_done(napi, 0);
		if (bhm_ipi_send(bh, preferred) != 0) {
			/* Dispatch failed (CPU raced offline, etc.). Re-kick
			 * locally so the BH doesn't stall. */
			napi_schedule(napi);
		}
		return 0;
	}

	work_done = bh->be.napi.user_poll(napi, budget);
	bhm_account_saturated(bh, work_done >= budget);
	return work_done;
}

static void bhm_tasklet_wrapper(unsigned long data)
{
	struct bhm_bh *bh = (struct bhm_bh *)data;

	bh->be.tasklet.user_fn(bh->be.tasklet.data);
	/* Drain/saturate reported explicitly via bhm_report_*. */
}

static void bhm_work_wrapper(struct work_struct *w)
{
	struct bhm_bh *bh = container_of(w, struct bhm_bh, be.work.w);

	bh->be.work.user_fn(w);
	/* Drain/saturate reported explicitly via bhm_report_*. */
}

/* -------------------------------------------------------------------------
 * 100ms sampler — aggregates per-netdev tput using rtnl_link_stats64
 * ---------------------------------------------------------------------- */

static void bhm_sample_tput_locked(u32 *global_tx_bps, u32 *global_rx_bps)
{
	struct bhm_netdev *e;
	struct rtnl_link_stats64 s;
	ktime_t now = ktime_get();
	u64 sum_tx_bps = 0, sum_rx_bps = 0;

	list_for_each_entry(e, &g_mgr.netdev_list, node) {
		s64 dt_ns;
		u64 dtx, drx, tx_bps, rx_bps;

		dev_get_stats(e->dev, &s);

		if (!e->primed) {
			e->last_tx_bytes = s.tx_bytes;
			e->last_rx_bytes = s.rx_bytes;
			e->last_sample   = now;
			e->tx_bps        = 0;
			e->rx_bps        = 0;
			e->primed        = true;
			continue;
		}

		dt_ns = ktime_to_ns(ktime_sub(now, e->last_sample));
		if (dt_ns <= 0) {
			/* Time went backwards or zero delta — skip. */
			e->last_tx_bytes = s.tx_bytes;
			e->last_rx_bytes = s.rx_bytes;
			e->last_sample   = now;
			e->tx_bps        = 0;
			e->rx_bps        = 0;
			continue;
		}

		dtx = s.tx_bytes - e->last_tx_bytes;
		drx = s.rx_bytes - e->last_rx_bytes;

		/* bytes * 8 * 1e9 / dt_ns = bits/sec. */
		tx_bps = (dtx * 8U * NSEC_PER_SEC) / (u64)dt_ns;
		rx_bps = (drx * 8U * NSEC_PER_SEC) / (u64)dt_ns;

		e->tx_bps = (tx_bps > U32_MAX) ? U32_MAX : (u32)tx_bps;
		e->rx_bps = (rx_bps > U32_MAX) ? U32_MAX : (u32)rx_bps;

		sum_tx_bps += e->tx_bps;
		sum_rx_bps += e->rx_bps;

		e->last_tx_bytes = s.tx_bytes;
		e->last_rx_bytes = s.rx_bytes;
		e->last_sample   = now;
	}

	*global_tx_bps = (sum_tx_bps > U32_MAX) ? U32_MAX : (u32)sum_tx_bps;
	*global_rx_bps = (sum_rx_bps > U32_MAX) ? U32_MAX : (u32)sum_rx_bps;
}

/* Compute @bh's filtered throughput aggregate. Caller must hold g_mgr.lock.
 * NULL filter → global aggregate (precomputed by bhm_sample_tput_locked).
 */
static void bhm_bh_tput_locked(const struct bhm_bh *bh,
				u32 *tx_bps, u32 *rx_bps)
{
	struct bhm_netdev *e;
	u64 sum_tx = 0, sum_rx = 0;
	const char * const *names = bh->netdev_names;

	if (!names) {
		*tx_bps = g_mgr.last_tput_tx;
		*rx_bps = g_mgr.last_tput_rx;
		return;
	}

	list_for_each_entry(e, &g_mgr.netdev_list, node) {
		const char * const *n;

		for (n = names; *n; n++) {
			if (strcmp(*n, e->dev->name) == 0) {
				sum_tx += e->tx_bps;
				sum_rx += e->rx_bps;
				break;
			}
		}
	}

	*tx_bps = (sum_tx > U32_MAX) ? U32_MAX : (u32)sum_tx;
	*rx_bps = (sum_rx > U32_MAX) ? U32_MAX : (u32)sum_rx;
}

static void bhm_evaluate_bh_locked(struct bhm_bh *bh)
{
	u32 tx, rx, selected;
	u32 evt = 0;

	bhm_bh_tput_locked(bh, &tx, &rx);
	selected = bhm_select_level(bh, tx, rx);

	if (selected == bh->current_level) {
		bh->candidate_level = bh->current_level;
		bh->candidate_ticks = 0;
	} else if (selected == bh->candidate_level) {
		u32 need = (selected > bh->current_level)
				? bh->hyst.rise_ticks : bh->hyst.fall_ticks;

		if (++bh->candidate_ticks >= need) {
			bh->current_level   = selected;
			bh->candidate_level = selected;
			bh->candidate_ticks = 0;
			evt |= BHM_EVT_LEVEL_TRANSITION;
		}
	} else {
		bh->candidate_level = selected;
		bh->candidate_ticks = 1;
	}

	if (bh->has_periodic && bh->current_level == 0)
		evt |= BHM_EVT_PERIODIC_TICK;

	if (evt) {
		bhm_latch_event(bh, evt, bh->current_level, tx, rx, bh->override_on);
		bhm_schedule_dispatch(bh);
	}
}

#if KERNEL_VERSION(4, 15, 0) <= LINUX_VERSION_CODE
static void bhm_timer_fn(struct timer_list *t)
#else
static void bhm_timer_fn(unsigned long data)
#endif
{
#if KERNEL_VERSION(4, 15, 0) <= LINUX_VERSION_CODE
	struct bhm_mgr *mgr = from_timer(mgr, t, timer);
#else
	struct bhm_mgr *mgr = (struct bhm_mgr *)data;
#endif
	struct bhm_bh *bh;
	unsigned long flags;
	u32 tx = 0, rx = 0;
	bool reschedule;

	spin_lock_irqsave(&mgr->lock, flags);
	bhm_sample_tput_locked(&tx, &rx);
	mgr->last_tput_tx = tx;
	mgr->last_tput_rx = rx;
	list_for_each_entry(bh, &mgr->bh_list, node)
		bhm_evaluate_bh_locked(bh);
	reschedule = !list_empty(&mgr->bh_list) && mgr->timer_armed;
	spin_unlock_irqrestore(&mgr->lock, flags);

	if (reschedule)
		mod_timer(&mgr->timer,
			  jiffies + msecs_to_jiffies(BHM_TIMER_PERIOD_MS));
}

static void bhm_arm_timer_if_needed_locked(void)
{
	if (g_mgr.timer_armed)
		return;
	if (list_empty(&g_mgr.bh_list))
		return;

	g_mgr.timer_armed = true;
	mod_timer(&g_mgr.timer,
		  jiffies + msecs_to_jiffies(BHM_TIMER_PERIOD_MS));
}

/* -------------------------------------------------------------------------
 * CPU hotplug integration (self-contained via cpuhp_setup_state)
 * ---------------------------------------------------------------------- */

static int bhm_cpuhp_online(unsigned int cpu)
{
	unsigned long flags;

	spin_lock_irqsave(&g_mgr.cpu_lock, flags);
	cpumask_set_cpu(cpu, &g_mgr.cpu_avail);
	spin_unlock_irqrestore(&g_mgr.cpu_lock, flags);
	return 0;
}

static int bhm_cpuhp_offline(unsigned int cpu)
{
	unsigned long flags;

	spin_lock_irqsave(&g_mgr.cpu_lock, flags);
	cpumask_clear_cpu(cpu, &g_mgr.cpu_avail);
	spin_unlock_irqrestore(&g_mgr.cpu_lock, flags);
	return 0;
}

static int bhm_cpuhp_register(void)
{
	int ret;

	/* The startup callback fires for every already-online CPU at setup
	 * time, so the mask is populated atomically with registration — no
	 * need to seed g_mgr.cpu_avail separately.
	 */
	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "net/bhm:online",
				bhm_cpuhp_online, bhm_cpuhp_offline);
	if (ret < 0)
		return ret;

	g_mgr.cpuhp_state      = (enum cpuhp_state)ret;
	g_mgr.cpuhp_registered = true;
	return 0;
}

static void bhm_cpuhp_unregister(void)
{
	if (!g_mgr.cpuhp_registered)
		return;
	cpuhp_remove_state(g_mgr.cpuhp_state);
	g_mgr.cpuhp_registered = false;
	g_mgr.cpuhp_state      = CPUHP_INVALID;
}

/* -------------------------------------------------------------------------
 * Netdev registry
 * ---------------------------------------------------------------------- */

int bhm_register_netdev(struct net_device *dev)
{
	struct bhm_netdev *e, *new_entry;
	unsigned long flags;

	if (!dev)
		return -EINVAL;

	new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
	if (!new_entry)
		return -ENOMEM;
	new_entry->dev = dev;

	spin_lock_irqsave(&g_mgr.lock, flags);
	list_for_each_entry(e, &g_mgr.netdev_list, node) {
		if (e->dev == dev) {
			spin_unlock_irqrestore(&g_mgr.lock, flags);
			kfree(new_entry);
			return -EEXIST;
		}
	}
	list_add_tail(&new_entry->node, &g_mgr.netdev_list);
	bhm_arm_timer_if_needed_locked();
	spin_unlock_irqrestore(&g_mgr.lock, flags);

	return 0;
}

int bhm_unregister_netdev(struct net_device *dev)
{
	struct bhm_netdev *e, *tmp, *found = NULL;
	unsigned long flags;

	if (!dev)
		return -EINVAL;

	spin_lock_irqsave(&g_mgr.lock, flags);
	list_for_each_entry_safe(e, tmp, &g_mgr.netdev_list, node) {
		if (e->dev == dev) {
			list_del(&e->node);
			found = e;
			break;
		}
	}
	spin_unlock_irqrestore(&g_mgr.lock, flags);

	if (!found)
		return -ENOENT;
	kfree(found);
	return 0;
}

/* -------------------------------------------------------------------------
 * Manager init / deinit
 * ---------------------------------------------------------------------- */

int bhm_init(void)
{
	int ret;

	if (g_mgr.initialized)
		return -EBUSY;

	memset(&g_mgr, 0, sizeof(g_mgr));
	spin_lock_init(&g_mgr.lock);
	spin_lock_init(&g_mgr.cpu_lock);
	INIT_LIST_HEAD(&g_mgr.bh_list);
	INIT_LIST_HEAD(&g_mgr.netdev_list);
	g_mgr.cpuhp_state = CPUHP_INVALID;

	g_mgr.wq = alloc_workqueue("bhm_wq",
				   WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!g_mgr.wq)
		return -ENOMEM;

#if KERNEL_VERSION(4, 15, 0) <= LINUX_VERSION_CODE
	timer_setup(&g_mgr.timer, bhm_timer_fn, 0);
#else
	setup_timer(&g_mgr.timer, bhm_timer_fn, (unsigned long)&g_mgr);
#endif

	init_dummy_netdev(&g_mgr.dummy_dev);

	ret = bhm_cpuhp_register();
	if (ret < 0) {
		destroy_workqueue(g_mgr.wq);
		g_mgr.wq = NULL;
		return ret;
	}

	g_mgr.initialized = true;
	return 0;
}

void bhm_deinit(void)
{
	struct bhm_netdev *e, *tmp;
	unsigned long flags;

	if (!g_mgr.initialized)
		return;

	bhm_cpuhp_unregister();

	g_mgr.timer_armed = false;
	del_timer_sync(&g_mgr.timer);

	if (g_mgr.wq) {
		flush_workqueue(g_mgr.wq);
		destroy_workqueue(g_mgr.wq);
		g_mgr.wq = NULL;
	}

	spin_lock_irqsave(&g_mgr.lock, flags);
	list_for_each_entry_safe(e, tmp, &g_mgr.netdev_list, node) {
		list_del(&e->node);
		kfree(e);
	}
	spin_unlock_irqrestore(&g_mgr.lock, flags);

	g_mgr.initialized = false;
}

/* -------------------------------------------------------------------------
 * Validation
 * ---------------------------------------------------------------------- */

static int bhm_validate_netdev_names(const char * const *names)
{
	const char * const *p;
	u32 count = 0;

	if (!names)
		return 0;   /* NULL → all registered netdevs */

	for (p = names; *p; p++) {
		size_t len = strlen(*p);

		if (len == 0 || len >= IFNAMSIZ) {
			pr_err("invalid netdev name length (must be 1..%d)\n",
			       IFNAMSIZ - 1);
			return -EINVAL;
		}
		count++;
	}

	if (count == 0) {
		pr_err("netdev_names is an empty array; use NULL for all\n");
		return -EINVAL;
	}

	return 0;
}

static int bhm_validate_levels(const struct bhm_params *p)
{
	u32 i;
	u32 prev = 0;
	bool prev_set = false;

	if (!p->levels || !p->nr_levels)
		return -EINVAL;

	for (i = 0; i < p->nr_levels; i++) {
		const struct bhm_level *lv = &p->levels[i];

		if (!lv->cb)
			return -EINVAL;

		if (bhm_level_is_periodic(lv)) {
			if (i != 0)
				return -EINVAL;   /* PERIODIC must be at index 0 */
			continue;
		}
		if (prev_set && lv->threshold_bps <= prev)
			return -EINVAL;       /* must be strictly ascending */
		prev = lv->threshold_bps;
		prev_set = true;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * BH register / unregister
 * ---------------------------------------------------------------------- */

static int bhm_alloc_per_cpu_slots(struct bhm_bh *bh)
{
	int cpu;

	bh->per_cpu = alloc_percpu(struct bhm_per_cpu);
	if (!bh->per_cpu)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		struct bhm_per_cpu *pc = per_cpu_ptr(bh->per_cpu, cpu);

		pc->bh        = bh;
		pc->csd.func  = bhm_csd_fn;
		pc->csd.info  = pc;
		pc->queued    = 0;
	}
	return 0;
}

static int bhm_bh_backend_init(struct bhm_bh *bh,
			      const struct bhm_params *p)
{
	int ret;

	switch (p->type) {
	case BHM_TYPE_NAPI:
	case BHM_TYPE_THREADED_NAPI: {
		struct net_device *dev = p->u.napi.dev;

		if (!p->u.napi.poll || !p->u.napi.weight)
			return -EINVAL;
		if (p->type == BHM_TYPE_THREADED_NAPI && !dev)
			return -EINVAL;
		if (!dev)
			dev = &g_mgr.dummy_dev;

		bh->be.napi.dev        = dev;
		bh->be.napi.user_poll  = p->u.napi.poll;
		bh->be.napi.weight     = p->u.napi.weight;
		bh->be.napi.threaded   = (p->type == BHM_TYPE_THREADED_NAPI);

		/* Only non-threaded NAPI uses IPI-based cross-CPU dispatch. */
		if (p->type == BHM_TYPE_NAPI) {
			ret = bhm_alloc_per_cpu_slots(bh);
			if (ret)
				return ret;
		}

#if KERNEL_VERSION(6, 1, 0) <= LINUX_VERSION_CODE
		netif_napi_add_weight(dev, &bh->be.napi.napi,
				      bhm_napi_poll_wrapper, p->u.napi.weight);
#else
		netif_napi_add(dev, &bh->be.napi.napi,
			       bhm_napi_poll_wrapper, p->u.napi.weight);
#endif
		napi_enable(&bh->be.napi.napi);
		bh->be.napi.added = true;

		if (bh->be.napi.threaded)
			dev_set_threaded(dev, true);
		break;
	}
	case BHM_TYPE_TASKLET:
		if (!p->u.tasklet.fn)
			return -EINVAL;
		bh->be.tasklet.user_fn = p->u.tasklet.fn;
		bh->be.tasklet.data    = p->u.tasklet.data;
		tasklet_init(&bh->be.tasklet.t, bhm_tasklet_wrapper,
			     (unsigned long)bh);
		ret = bhm_alloc_per_cpu_slots(bh);
		if (ret) {
			tasklet_kill(&bh->be.tasklet.t);
			return ret;
		}
		break;
	case BHM_TYPE_WORKQUEUE:
		if (!p->u.work.fn)
			return -EINVAL;
		bh->be.work.user_fn = p->u.work.fn;
		bh->be.work.wq      = p->u.work.wq;   /* may be NULL */
		INIT_WORK(&bh->be.work.w, bhm_work_wrapper);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void bhm_drain_pending_csds(struct bhm_bh *bh)
{
	int cpu;

	if (!bh->per_cpu)
		return;

	/* A CSD in flight will set `queued=1` until bhm_csd_fn clears it as
	 * its last operation. Busy-wait briefly until every slot drains so
	 * we don't free the bh while a CSD still dereferences it.
	 */
	for_each_possible_cpu(cpu) {
		struct bhm_per_cpu *pc = per_cpu_ptr(bh->per_cpu, cpu);

		while (test_bit(0, &pc->queued))
			cpu_relax();
	}
}

static void bhm_bh_backend_deinit(struct bhm_bh *bh)
{
	switch (bh->type) {
	case BHM_TYPE_NAPI:
	case BHM_TYPE_THREADED_NAPI:
		if (bh->be.napi.added) {
			napi_disable(&bh->be.napi.napi);
			netif_napi_del(&bh->be.napi.napi);
			bh->be.napi.added = false;
		}
		break;
	case BHM_TYPE_TASKLET:
		tasklet_kill(&bh->be.tasklet.t);
		break;
	case BHM_TYPE_WORKQUEUE:
		cancel_work_sync(&bh->be.work.w);
		break;
	}

	bhm_drain_pending_csds(bh);
	if (bh->per_cpu) {
		free_percpu(bh->per_cpu);
		bh->per_cpu = NULL;
	}
}

struct bhm_bh *bhm_register(const struct bhm_params *params,
				 void *priv)
{
	struct bhm_bh *bh;
	unsigned long flags;
	int ret;
	size_t sz;

	if (!g_mgr.initialized || !params)
		return ERR_PTR(-EINVAL);

	ret = bhm_validate_levels(params);
	if (ret)
		return ERR_PTR(ret);

	ret = bhm_validate_netdev_names(params->netdev_names);
	if (ret)
		return ERR_PTR(ret);

	bh = kzalloc(sizeof(*bh), GFP_KERNEL);
	if (!bh)
		return ERR_PTR(-ENOMEM);

	sz = sizeof(*params->levels) * params->nr_levels;
	bh->levels = kmalloc(sz, GFP_KERNEL);
	if (!bh->levels) {
		kfree(bh);
		return ERR_PTR(-ENOMEM);
	}
	memcpy(bh->levels, params->levels, sz);

	bh->priv            = priv;
	bh->type            = params->type;
	bh->nr_levels       = params->nr_levels;
	bh->has_periodic    = bhm_level_is_periodic(&bh->levels[0]);
	bh->hyst.rise_ticks = params->hyst.rise_ticks ? : BHM_DEFAULT_RISE_TICKS;
	bh->hyst.fall_ticks = params->hyst.fall_ticks ? : BHM_DEFAULT_FALL_TICKS;
	bh->ovcfg           = params->override;
	bh->netdev_names    = params->netdev_names;
	bh->current_level   = 0;
	bh->candidate_level = 0;
	bh->candidate_ticks = 0;
	cpumask_clear(&bh->preferred_mask);

	INIT_WORK(&bh->dispatch_work, bhm_dispatch_work_fn);
	INIT_DELAYED_WORK(&bh->override_clear_work, bhm_override_clear_work_fn);

	ret = bhm_bh_backend_init(bh, params);
	if (ret) {
		kfree(bh->levels);
		kfree(bh);
		return ERR_PTR(ret);
	}

	spin_lock_irqsave(&g_mgr.lock, flags);
	list_add_tail(&bh->node, &g_mgr.bh_list);
	bhm_arm_timer_if_needed_locked();
	spin_unlock_irqrestore(&g_mgr.lock, flags);

	return bh;
}

int bhm_unregister(struct bhm_bh *bh)
{
	unsigned long flags;
	bool need_timer_kill;

	if (!bh)
		return -EINVAL;

	spin_lock_irqsave(&g_mgr.lock, flags);
	list_del(&bh->node);
	need_timer_kill = list_empty(&g_mgr.bh_list);
	if (need_timer_kill)
		g_mgr.timer_armed = false;
	spin_unlock_irqrestore(&g_mgr.lock, flags);

	if (need_timer_kill)
		del_timer_sync(&g_mgr.timer);

	cancel_delayed_work_sync(&bh->override_clear_work);
	cancel_work_sync(&bh->dispatch_work);

	bhm_bh_backend_deinit(bh);

	kfree(bh->levels);
	kfree(bh);
	return 0;
}

int bhm_schedule(struct bhm_bh *bh)
{
	if (!bh)
		return -EINVAL;

	switch (bh->type) {
	case BHM_TYPE_NAPI:
		/* Preferred-CPU handling happens in the poll wrapper, not here
		 * — napi_schedule runs on whatever CPU the IRQ was on; the
		 * wrapper will migrate if needed.
		 */
	case BHM_TYPE_THREADED_NAPI:
		napi_schedule(&bh->be.napi.napi);
		return 0;

	case BHM_TYPE_TASKLET: {
		int preferred;

		if (bhm_should_use_ipi(bh, &preferred) &&
		    bhm_ipi_send(bh, preferred) == 0)
			return 0;              /* handed off via IPI */
		tasklet_schedule(&bh->be.tasklet.t);
		return 0;
	}

	case BHM_TYPE_WORKQUEUE: {
		struct workqueue_struct *wq = bh->be.work.wq ? : system_wq;
		int picked = bhm_resolve_preferred_cpu(bh);

		if (picked >= 0)
			queue_work_on(picked, wq, &bh->be.work.w);
		else
			queue_work(wq, &bh->be.work.w);
		return 0;
	}
	}
	return -EINVAL;
}

/* -------------------------------------------------------------------------
 * Public: preferred-CPU
 * ---------------------------------------------------------------------- */

/* Capacity + idle-aware CPU picker (big.LITTLE friendly).
 *
 *   1. Walk mask ∩ online. Among idle CPUs, pick the one with the highest
 *      arch_scale_cpu_capacity — i.e. prefer a big-cluster idle core over
 *      a little-cluster idle core when both are available.
 *   2. If no CPU is idle, round-robin across mask ∩ online via
 *      cpumask_any_and_distribute so repeated dispatches spread out.
 *
 * Returns -1 if the mask has no online member.
 */
static int bhm_pick_capacity_aware(const struct cpumask *mask)
{
	unsigned int cpu;
	unsigned int best = nr_cpu_ids;
	unsigned long best_cap = 0;
	unsigned int rr;

	for_each_cpu_and(cpu, mask, cpu_online_mask) {
		unsigned long cap;

		if (!available_idle_cpu(cpu))
			continue;
		cap = arch_scale_cpu_capacity(cpu);
		if (cap > best_cap) {
			best_cap = cap;
			best     = cpu;
		}
	}
	if (best < nr_cpu_ids)
		return (int)best;

	rr = cpumask_any_and_distribute(mask, cpu_online_mask);
	return (rr < nr_cpu_ids) ? (int)rr : -1;
}

/* Resolve where this BH should run right now:
 *   - no preference        → -1
 *   - single-bit mask      → that bit (if online; no analysis needed)
 *   - current CPU allowed  → current CPU (short-circuit; no IPI needed)
 *   - else                 → capacity-aware pick from mask ∩ online
 */
static int bhm_resolve_preferred_cpu(struct bhm_bh *bh)
{
	int self;
	unsigned int cpu;

	if (cpumask_empty(&bh->preferred_mask))
		return -1;

	/* Fast path: mask has a single bit set — nothing to choose. */
	if (cpumask_weight(&bh->preferred_mask) == 1) {
		cpu = cpumask_first(&bh->preferred_mask);
		return (cpu < nr_cpu_ids && cpu_online(cpu)) ? (int)cpu : -1;
	}

	self = smp_processor_id();
	if (cpumask_test_cpu(self, &bh->preferred_mask) && cpu_online(self))
		return self;

	return bhm_pick_capacity_aware(&bh->preferred_mask);
}

int bhm_set_preferred_cpumask(struct bhm_bh *bh, const struct cpumask *mask)
{
	unsigned long flags;
	cpumask_t snapshot;

	if (!bh)
		return -EINVAL;

	/* Store the new mask under the manager lock so concurrent setters
	 * don't interleave. Snapshot it for the kthread-affinity call below
	 * (which may sleep and must not be made under a spinlock).
	 */
	spin_lock_irqsave(&g_mgr.lock, flags);
	if (mask)
		cpumask_copy(&bh->preferred_mask, mask);
	else
		cpumask_clear(&bh->preferred_mask);
	cpumask_copy(&snapshot, &bh->preferred_mask);
	spin_unlock_irqrestore(&g_mgr.lock, flags);

	/* For THREADED_NAPI, apply the mask directly to the kthread.
	 * Empty snapshot → allow all CPUs so the kthread isn't stranded.
	 */
	if (bh->type == BHM_TYPE_THREADED_NAPI) {
		struct task_struct *thread = bh->be.napi.napi.thread;

		if (thread) {
			if (cpumask_empty(&snapshot))
				set_cpus_allowed_ptr(thread, cpu_possible_mask);
			else
				set_cpus_allowed_ptr(thread, &snapshot);
		}
	}

	return 0;
}

int bhm_set_preferred_cpu(struct bhm_bh *bh, int cpu)
{
	cpumask_t mask;

	if (!bh)
		return -EINVAL;
	if (cpu >= (int)nr_cpu_ids)
		return -EINVAL;

	if (cpu < 0)
		return bhm_set_preferred_cpumask(bh, NULL);

	cpumask_clear(&mask);
	cpumask_set_cpu(cpu, &mask);
	return bhm_set_preferred_cpumask(bh, &mask);
}

/* -------------------------------------------------------------------------
 * Public: saturation / override / queries
 * ---------------------------------------------------------------------- */

void bhm_report_saturated(struct bhm_bh *bh)
{
	if (bh)
		bhm_account_saturated(bh, true);
}

void bhm_report_drained(struct bhm_bh *bh)
{
	if (bh)
		bhm_account_saturated(bh, false);
}

void bhm_force_override(struct bhm_bh *bh)
{
	unsigned long flags;

	if (!bh)
		return;
	spin_lock_irqsave(&g_mgr.lock, flags);
	bhm_trigger_override_locked(bh);
	spin_unlock_irqrestore(&g_mgr.lock, flags);
}

void bhm_clear_override(struct bhm_bh *bh)
{
	unsigned long flags;
	bool fire = false;

	if (!bh)
		return;
	spin_lock_irqsave(&g_mgr.lock, flags);
	if (bh->override_on) {
		u32 tx, rx;

		bh->override_on = false;
		bh->sat_streak  = 0;
		bhm_bh_tput_locked(bh, &tx, &rx);
		bhm_latch_event(bh, BHM_EVT_OVERRIDE_EDGE, bh->current_level,
			       tx, rx, false);
		fire = true;
	}
	spin_unlock_irqrestore(&g_mgr.lock, flags);

	cancel_delayed_work(&bh->override_clear_work);
	if (fire)
		bhm_schedule_dispatch(bh);
}

bool bhm_is_override(struct bhm_bh *bh)
{
	unsigned long flags;
	bool r;

	if (!bh)
		return false;
	spin_lock_irqsave(&g_mgr.lock, flags);
	r = bh->override_on;
	spin_unlock_irqrestore(&g_mgr.lock, flags);
	return r;
}

u32 bhm_current_level(struct bhm_bh *bh)
{
	unsigned long flags;
	u32 r;

	if (!bh)
		return BHM_INVALID_LEVEL;
	spin_lock_irqsave(&g_mgr.lock, flags);
	r = bh->current_level;
	spin_unlock_irqrestore(&g_mgr.lock, flags);
	return r;
}

void *bhm_priv(struct bhm_bh *bh)
{
	return bh ? bh->priv : NULL;
}

void bhm_get_tput(u32 *tx_bps, u32 *rx_bps)
{
	unsigned long flags;

	spin_lock_irqsave(&g_mgr.lock, flags);
	if (tx_bps) *tx_bps = g_mgr.last_tput_tx;
	if (rx_bps) *rx_bps = g_mgr.last_tput_rx;
	spin_unlock_irqrestore(&g_mgr.lock, flags);
}

struct bhm_bh *bhm_napi_to_bh(struct napi_struct *napi)
{
	return container_of(napi, struct bhm_bh, be.napi.napi);
}

struct bhm_bh *bhm_work_to_bh(struct work_struct *work)
{
	return container_of(work, struct bhm_bh, be.work.w);
}

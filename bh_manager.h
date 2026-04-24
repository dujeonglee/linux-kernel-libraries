/* SPDX-License-Identifier: GPL-2.0 */
/******************************************************************************
 *
 * Copyright (C) 2026 Dujeong Lee <dujeong.lee82@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * bh_manager — unified BH manager (self-contained)
 *
 *   Responsibilities:
 *     - Own bottom-half lifecycles (workqueue / tasklet / napi / threaded_napi)
 *     - Sample aggregated TX/RX throughput of all registered netdevices every
 *       100ms using standard rtnl_link_stats64
 *     - Track CPU availability via cpu-hotplug notifications
 *     - Dispatch per-BH level callbacks on:
 *         (a) level-transition edges (with per-BH hysteresis)
 *         (b) periodic ticks while a PERIODIC level is active
 *         (c) override set/clear edges
 *     - Detect override via streaks of work_done == budget (napi/threaded_napi)
 *       or via explicit bhm_report_saturated() (all BH types)
 *
 *   This module has no dependency on slsi_dev / netdev_vif or any other
 *   driver-internal structure. Callers register each active net_device via
 *   bhm_register_netdev() and retract it with bhm_unregister_netdev().
 *
 *   Affinity / RPS / per-netdev NAPI enable policy is the caller's business —
 *   this module only reports (level, tput_tx, tput_rx, override, avail_cpus).
 *
 * -------------------------------------------------------------------------
 *  Usage overview
 * -------------------------------------------------------------------------
 *
 *  Lifecycle:
 *
 *      bhm_init();                       // once, at driver probe
 *      bhm_register_netdev(dev);             // per netdev when it goes up
 *      bh = bhm_register(&params, priv);     // per BH
 *      ...
 *      bhm_unregister(bh);
 *      bhm_unregister_netdev(dev);           // before dev is freed
 *      bhm_deinit();                     // once, at driver remove
 *
 *  Threshold values are in bits per second (bps). A level whose threshold
 *  is BHM_LEVEL_PERIODIC is a floor-level that also fires every tick
 *  while active. A "plain" floor with threshold_bps == 0 fires only on
 *  entry.
 *
 * -------------------------------------------------------------------------
 *  Example 1: NAPI with 3 tput levels, override via full-budget streak
 * -------------------------------------------------------------------------
 *
 *      static void my_cb(struct bhm_bh *bh, u32 level,
 *                        u32 tx, u32 rx, bool override,
 *                        const cpumask_t *avail, void *ctx)
 *      {
 *              // Caller decides policy. E.g. pick a CPU from `avail` and
 *              // set napi affinity; kick RPS; enable threaded mode, etc.
 *              pr_info("bh=%p level=%u tx=%u rx=%u ov=%d\n",
 *                      bh, level, tx, rx, override);
 *      }
 *
 *      static const struct bhm_level lv[] = {
 *              { .threshold_bps = 0,              .dir = BHM_DIR_BOTH, .cb = my_cb },
 *              { .threshold_bps = 100 * 1000000U, .dir = BHM_DIR_BOTH, .cb = my_cb },
 *              { .threshold_bps = 500 * 1000000U, .dir = BHM_DIR_BOTH, .cb = my_cb },
 *      };
 *
 *      static const char * const my_dev_filter[] = { "wlan0", NULL };
 *
 *      const struct bhm_params p = {
 *              .type = BHM_TYPE_NAPI,
 *              .u.napi = { .poll = my_poll, .weight = NAPI_POLL_WEIGHT, .dev = my_dev },
 *              .levels = lv, .nr_levels = ARRAY_SIZE(lv),
 *              .hyst   = { .rise_ticks = 1, .fall_ticks = 10 },
 *              .override = { .budget_full_streak = 4, .timeout_ms = 1000 },
 *              .netdev_names = my_dev_filter,   // optional; NULL = ALL
 *      };
 *
 *      struct bhm_bh *bh = bhm_register(&p, my_priv);
 *
 *      // napi wrapper counts consecutive work_done==budget automatically;
 *      // after 4 such runs, override latches. Auto-clears after 1000 ms.
 *
 * -------------------------------------------------------------------------
 *  Example 2: Workqueue with a single PERIODIC level
 * -------------------------------------------------------------------------
 *
 *  Every 100 ms `tick_cb` fires regardless of tput — useful for pure
 *  monitoring / logging clients.
 *
 *      static const struct bhm_level lv[] = {
 *              { .threshold_bps = BHM_LEVEL_PERIODIC,
 *                .dir = BHM_DIR_BOTH, .cb = tick_cb },
 *      };
 *
 *      const struct bhm_params p = {
 *              .type = BHM_TYPE_WORKQUEUE,
 *              .u.work = { .fn = my_work_fn, .wq = NULL },  // NULL → system_wq
 *              .levels = lv, .nr_levels = 1,
 *              // hyst / override can be left zero-initialized.
 *      };
 *
 * -------------------------------------------------------------------------
 *  Example 3: Tasklet with mixed directions + explicit saturation report
 * -------------------------------------------------------------------------
 *
 *  TX-heavy level triggers on TX alone; RX-heavy on RX alone. Tasklets
 *  don't have a NAPI-style budget, so the handler reports saturation
 *  explicitly when it hits its internal work limit.
 *
 *      static const struct bhm_level lv[] = {
 *              { .threshold_bps = 0,              .dir = BHM_DIR_BOTH, .cb = floor_cb },
 *              { .threshold_bps = 200 * 1000000U, .dir = BHM_DIR_RX,   .cb = rx_cb    },
 *              { .threshold_bps = 400 * 1000000U, .dir = BHM_DIR_TX,   .cb = tx_cb    },
 *      };
 *
 *      struct my_ctx {
 *              struct bhm_bh *bh;      // filled in after bhm_register
 *              // ... other caller state ...
 *      };
 *      static struct my_ctx ctx;
 *
 *      static void my_tasklet(unsigned long data)
 *      {
 *              struct my_ctx *c = (struct my_ctx *)data;
 *              int processed = drain_up_to(MAX_PER_RUN);
 *
 *              if (processed == MAX_PER_RUN && still_work_pending())
 *                      bhm_report_saturated(c->bh);
 *              else
 *                      bhm_report_drained(c->bh);
 *      }
 *
 *      const struct bhm_params p = {
 *              .type = BHM_TYPE_TASKLET,
 *              .u.tasklet = { .fn = my_tasklet, .data = (unsigned long)&ctx },
 *              .levels = lv, .nr_levels = ARRAY_SIZE(lv),
 *              .override = { .budget_full_streak = 3, .timeout_ms = 500 },
 *      };
 *
 *      ctx.bh = bhm_register(&p, &ctx);   // back-pointer for the handler
 *
 * -------------------------------------------------------------------------
 *  Example 4: Threaded NAPI (dev->threaded = 1)
 * -------------------------------------------------------------------------
 *
 *      const struct bhm_params p = {
 *              .type = BHM_TYPE_THREADED_NAPI,
 *              .u.napi = { .poll = my_poll, .weight = 64, .dev = my_dev },
 *              .levels = lv, .nr_levels = ARRAY_SIZE(lv),
 *              .override = { .budget_full_streak = 5, .timeout_ms = 2000 },
 *      };
 *
 *      // `dev` is mandatory for THREADED variant — dev_set_threaded(dev, 1)
 *      // is applied by the manager at registration.
 *
 * -------------------------------------------------------------------------
 *  Callback behavior reference
 * -------------------------------------------------------------------------
 *
 *  For each BH, at any point in time exactly one level is "active". The
 *  callback bound to the active level fires in these situations:
 *
 *      - Level transition (entry): after hysteresis passes, the entered
 *        level's cb is invoked once with the current (tx, rx, override).
 *
 *      - Periodic tick: if the active level's threshold_bps equals
 *        BHM_LEVEL_PERIODIC, its cb fires every 100 ms while active.
 *
 *      - Override edge: on override set/clear (whether automatic via
 *        budget-full streak, timeout, or explicit via bhm_*_override),
 *        the currently active level's cb is invoked once with the new
 *        override flag.
 *
 *  The manager never performs any tput-level policy of its own. It only
 *  reports state — your callback decides what to do (CPU affinity, RPS
 *  map, threaded mode, IRQ coalescing, etc).
 *
 *****************************************************************************/

#ifndef __BH_MANAGER_H__
#define __BH_MANAGER_H__

#include <linux/cpumask.h>
#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct bhm_bh;

enum bhm_type {
	BHM_TYPE_WORKQUEUE,
	BHM_TYPE_TASKLET,
	BHM_TYPE_NAPI,
	BHM_TYPE_THREADED_NAPI,
};

enum bhm_tput_dir {
	BHM_DIR_TX,
	BHM_DIR_RX,
	BHM_DIR_BOTH,
};

/* Sentinel for bhm_level::threshold_bps.
 * Semantics: the level has no tput threshold; it is always treated as the
 * floor (active only when no higher threshold level matches) and its
 * callback fires every 100ms while active.
 * At most one PERIODIC level per BH, placed at index 0 of levels[].
 */
#define BHM_LEVEL_PERIODIC  ((u32)-1)

/* Callback invocation contract:
 *   - Called from a dedicated manager workqueue (sleepable).
 *   - Invoked on (a) transition INTO this level, (b) override edge while
 *     this level is active, (c) every 100ms tick if this level is PERIODIC
 *     and currently active.
 *   - (a) and (b) use net-change semantics: the callback fires only when
 *     the delivered state differs from what the consumer was last told.
 *     A burst that nets to no change (e.g. off→on→off within one dispatch
 *     cycle) produces no callback. Periodic ticks (c) always pass through.
 *   - avail_cpus points to an internal snapshot; copy with cpumask_copy()
 *     if the callback needs to retain it beyond the call.
 */
typedef void (*bhm_level_cb)(struct bhm_bh *bh,
				 u32 level_idx,
				 u32 tput_tx_bps,
				 u32 tput_rx_bps,
				 bool override,
				 const cpumask_t *avail_cpus,
				 void *ctx);

struct bhm_level {
	u32                   threshold_bps;   /* BHM_LEVEL_PERIODIC or bps */
	enum bhm_tput_dir    dir;
	bhm_level_cb      cb;
	void                 *ctx;
};

struct bhm_hysteresis {
	u32 rise_ticks;   /* ticks required to confirm an upward transition  */
	u32 fall_ticks;   /* ticks required to confirm a downward transition */
};

struct bhm_override_cfg {
	/* Consecutive full-budget NAPI runs (or explicit saturated reports)
	 * required to latch override. 0 disables automatic override.
	 */
	u32 budget_full_streak;

	/* Auto-clear override after this many milliseconds.
	 * 0 means no auto-clear (manual only via bhm_clear_override()).
	 */
	u32 timeout_ms;
};

struct bhm_napi_params {
	int                (*poll)(struct napi_struct *napi, int budget);
	int                  weight;
	/* Optional. If NULL, the manager's internal dummy netdev is used.
	 * Mandatory for BHM_TYPE_THREADED_NAPI (dev->threaded is set).
	 */
	struct net_device   *dev;
};

struct bhm_tasklet_params {
	void           (*fn)(unsigned long data);
	unsigned long    data;
};

struct bhm_workqueue_params {
	void                      (*fn)(struct work_struct *work);
	struct workqueue_struct   *wq;     /* NULL → system_wq */
};

struct bhm_params {
	enum bhm_type type;

	union {
		struct bhm_napi_params       napi;     /* NAPI / THREADED_NAPI */
		struct bhm_tasklet_params    tasklet;
		struct bhm_workqueue_params  work;
	} u;

	/* Levels must be ascending by threshold_bps. A PERIODIC level, if
	 * present, must be at index 0.
	 */
	const struct bhm_level      *levels;
	u32                              nr_levels;

	struct bhm_hysteresis        hyst;
	struct bhm_override_cfg      override;

	/* Optional: NULL-terminated array of netdev interface names whose
	 * aggregated throughput drives this BH's level selection and the
	 * tput values reported to the callback.
	 *
	 *   NULL (default) → sum across ALL registered netdevs.
	 *   { "wlan0", NULL }            → watch wlan0 only.
	 *   { "wlan0", "wlan1", NULL }   → watch both interfaces.
	 *
	 * An empty array ({ NULL }) is rejected — use NULL to mean "all".
	 * The pointer and the strings must outlive the BH (string literals
	 * or static storage are the common choice). Names longer than
	 * IFNAMSIZ-1 are rejected at registration.
	 */
	const char * const          *netdev_names;
};

/* ---------- Manager lifecycle ---------- */

int  bhm_init(void);
void bhm_deinit(void);

/* ---------- Active netdev registry ----------
 *
 * The manager aggregates TX/RX throughput across every registered netdev.
 * Callers must register each active interface before throughput monitoring
 * will count it, and unregister it before the net_device is torn down.
 *
 * Safe to call before or after bhm_init (registration persists).
 * Duplicate registration returns -EEXIST. Unregistering an unknown dev
 * returns -ENOENT.
 */
int  bhm_register_netdev(struct net_device *dev);
int  bhm_unregister_netdev(struct net_device *dev);

/* ---------- BH lifecycle ---------- */

struct bhm_bh *bhm_register(const struct bhm_params *params,
				 void *priv);
int  bhm_unregister(struct bhm_bh *bh);

/* Request the BH to run once. Returns 0 on success. */
int  bhm_schedule(struct bhm_bh *bh);

/* ---------- Preferred-CPU dispatch ----------
 *
 * Binds the BH's execution to a set of CPUs. The mask is interpreted per
 * BH type:
 *
 *   THREADED_NAPI:
 *     The NAPI kthread's allowed CPU set is replaced with @mask via
 *     set_cpus_allowed_ptr. The kernel scheduler places the kthread on
 *     any CPU in the mask as it sees fit. Multi-CPU masks are natural —
 *     e.g. pin to a cluster of big cores.
 *
 *   NAPI (non-threaded) / TASKLET / WORKQUEUE:
 *     The placement is resolved at each dispatch (no cached single CPU):
 *
 *       1. If the current CPU is inside @mask, stay — honors whoever
 *          delivered the IRQ/event here (IRQ affinity, softirq locality)
 *          and keeps cache warm.
 *       2. Else pick the HIGHEST-CAPACITY idle CPU within the mask —
 *          big.LITTLE aware via arch_scale_cpu_capacity(). On mobile
 *          SoCs this means: if the user's mask spans a big cluster and
 *          any of those cores are idle, the big core wins; otherwise
 *          falls back to any idle CPU in the mask.
 *       3. Else (nobody idle) fair-distribute via
 *          cpumask_any_and_distribute() so sustained load spreads out.
 *
 *     NAPI redirects via smp_call_function_single_async (IPI) from the
 *     poll wrapper; TASKLET uses the same IPI from bhm_schedule();
 *     WORKQUEUE calls queue_work_on(picked, ...).
 *
 *     If the mask contains only a single bit, all of the above is
 *     short-circuited to "use that bit (if online)". No capacity/idle
 *     analysis runs for single-CPU masks.
 *
 * Pass @mask = NULL or an empty mask to clear the preference (run
 * wherever scheduling lands / allow the kthread on all CPUs).
 *
 * The single-CPU convenience wrapper bhm_set_preferred_cpu() is
 * equivalent to calling the mask form with a single-bit mask for cpu >= 0
 * or NULL for cpu == -1.
 *
 * Both forms are safe to call from the level-callback context and may be
 * called repeatedly.
 */
int  bhm_set_preferred_cpumask(struct bhm_bh *bh,
				   const struct cpumask *mask);
int  bhm_set_preferred_cpu(struct bhm_bh *bh, int cpu);

/* ---------- Saturation / override controls ---------- */

/* For tasklet/workqueue: call from inside the handler when the BH hit its
 * internal bail-out limit and still has work queued. Also overridable for
 * napi/threaded_napi if the automatic work_done==budget detection is not
 * sufficient.
 */
void bhm_report_saturated(struct bhm_bh *bh);
void bhm_report_drained(struct bhm_bh *bh);

/* Manual override control. force_override latches override immediately;
 * clear_override releases it (subject to timeout logic resetting it again
 * if saturation continues).
 */
void bhm_force_override(struct bhm_bh *bh);
void bhm_clear_override(struct bhm_bh *bh);
bool bhm_is_override(struct bhm_bh *bh);

/* ---------- Queries ---------- */

/* Snapshot the currently-active level index. */
u32  bhm_current_level(struct bhm_bh *bh);

/* Retrieve the caller-supplied priv pointer passed to bhm_register. */
void *bhm_priv(struct bhm_bh *bh);

/* Snapshot aggregated tput (bps) across all registered netdevs. */
void bhm_get_tput(u32 *tx_bps, u32 *rx_bps);

/* ---------- Backend-to-BH accessors ----------
 *
 * Recover the BH handle from the underlying backend object inside user
 * handlers that receive only the backend pointer.
 *
 *   NAPI poll:
 *       static int my_poll(struct napi_struct *napi, int budget) {
 *           struct bhm_bh *bh = bhm_napi_to_bh(napi);
 *           int done = drain(napi, budget);
 *           if (done < budget)
 *               bhm_report_drained(bh);
 *           return done;
 *       }
 *
 *   Workqueue:
 *       static void my_work(struct work_struct *w) {
 *           struct bhm_bh *bh = bhm_work_to_bh(w);
 *           struct my_ctx *c = bhm_priv(bh);
 *           ...
 *       }
 *
 * Tasklet has no generic accessor — handlers receive the caller-supplied
 * `unsigned long data`, not a tasklet_struct. Stash the BH pointer in
 * your ctx struct after bhm_register() returns and pass the ctx as data:
 *
 *       struct my_ctx {
 *           struct bhm_bh *bh;        // filled in after register
 *           // ... other state ...
 *       };
 *
 *       static void my_tasklet(unsigned long data) {
 *           struct my_ctx *c = (struct my_ctx *)data;
 *           int processed = drain_up_to(MAX_PER_RUN);
 *
 *           if (processed == MAX_PER_RUN && still_work_pending())
 *                   bhm_report_saturated(c->bh);
 *           else
 *                   bhm_report_drained(c->bh);
 *       }
 *
 *       static struct my_ctx ctx;
 *       const struct bhm_params p = {
 *           .type = BHM_TYPE_TASKLET,
 *           .u.tasklet = { .fn = my_tasklet, .data = (unsigned long)&ctx },
 *           // ...
 *       };
 *       ctx.bh = bhm_register(&p, &ctx);   // back-pointer for the handler
 *
 * These accept only backend pointers that actually belong to a BH
 * registered with the matching type. Passing an unrelated pointer is
 * undefined behavior (container_of returns garbage).
 */
struct bhm_bh *bhm_napi_to_bh(struct napi_struct *napi);
struct bhm_bh *bhm_work_to_bh(struct work_struct *work);

#endif /* __BH_MANAGER_H__ */

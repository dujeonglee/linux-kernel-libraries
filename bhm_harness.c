// SPDX-License-Identifier: GPL-2.0
/*
 * bh_manager test harness.
 *
 *   Creates two fake netdevs (bhmtest0, bhmtest1) whose byte counters the
 *   harness drives, and registers four BHs that exercise different
 *   framework features. Logs every callback invocation with a millisecond
 *   timestamp relative to module load, tagged with which BH fired.
 *
 *   SCENARIO 1 — single BH + hysteresis (bh_global)
 *     drive bhmtest0 from 0 → 150Mbps → 600Mbps → idle. Expect L0→L1
 *     rise within one tick (rise_ticks=1), L1→L2 rise within one tick,
 *     L2→L0 fall after ~1s (fall_ticks=10).
 *
 *   SCENARIO 2 — per-BH netdev filter (bh_wlan0 vs bh_global)
 *     drive bhmtest1 only. bh_global (no filter) sees aggregate and
 *     rises; bh_wlan0 (filter=[bhmtest0]) sees 0 and stays at L0.
 *
 *   SCENARIO 3 — net-change filter on override (bh_override)
 *     rapid force→clear within one dispatch window collapses into
 *     no callback. Subsequent stable force fires once, stable clear
 *     fires once.
 *
 *   SCENARIO 4 — bhm_work_to_bh accessor (bh_accessor)
 *     bhm_schedule() invokes the user's work fn, which recovers the
 *     bh handle via bhm_work_to_bh() and queries bhm_current_level().
 */

#define pr_fmt(fmt) "bhm_test: " fmt

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/slab.h>

#include "bh_manager.h"

#define L1_BPS (100U * 1000000U)
#define L2_BPS (500U * 1000000U)

static struct net_device *tdev0, *tdev1;
static struct bhm_bh     *bh_global, *bh_wlan0, *bh_override, *bh_accessor;
static struct task_struct *scenario_task;
static ktime_t test_start;

static const char * const filter_wlan0[] = { "bhmtest0", NULL };

static s64 now_ms(void)
{
	return ktime_ms_delta(ktime_get(), test_start);
}

/* Generic callback; @ctx identifies which BH fired. */
static void cb_common(struct bhm_bh *bh, u32 level,
		       u32 tx, u32 rx, bool override,
		       const cpumask_t *avail, void *ctx)
{
	const char *tag = ctx;

	pr_info("cb[%-8s] t=%lldms level=%u tx=%uMbps rx=%uMbps ov=%d\n",
		tag, now_ms(), level, tx / 1000000U, rx / 1000000U, override);
}

static void noop_work_fn(struct work_struct *w)
{
}

/* Demonstrates bhm_work_to_bh(): recovers bh from work, reads level. */
static void accessor_work_fn(struct work_struct *w)
{
	struct bhm_bh *bh = bhm_work_to_bh(w);

	pr_info("accessor_fn: t=%lldms bh=%p current_level=%u\n",
		now_ms(), bh, bhm_current_level(bh));
}

/* Three-tier level sets; one per identifying ctx tag. */
static const struct bhm_level levels_global[] = {
	{ .threshold_bps = 0,      .dir = BHM_DIR_BOTH, .cb = cb_common, .ctx = "global" },
	{ .threshold_bps = L1_BPS, .dir = BHM_DIR_BOTH, .cb = cb_common, .ctx = "global" },
	{ .threshold_bps = L2_BPS, .dir = BHM_DIR_BOTH, .cb = cb_common, .ctx = "global" },
};

static const struct bhm_level levels_wlan0[] = {
	{ .threshold_bps = 0,      .dir = BHM_DIR_BOTH, .cb = cb_common, .ctx = "wlan0" },
	{ .threshold_bps = L1_BPS, .dir = BHM_DIR_BOTH, .cb = cb_common, .ctx = "wlan0" },
	{ .threshold_bps = L2_BPS, .dir = BHM_DIR_BOTH, .cb = cb_common, .ctx = "wlan0" },
};

static const struct bhm_level levels_single_override[] = {
	{ .threshold_bps = 0, .dir = BHM_DIR_BOTH, .cb = cb_common, .ctx = "override" },
};

static const struct bhm_level levels_single_accessor[] = {
	{ .threshold_bps = 0, .dir = BHM_DIR_BOTH, .cb = cb_common, .ctx = "access" },
};

static void test_ndo_get_stats64(struct net_device *dev,
				  struct rtnl_link_stats64 *stats)
{
	stats->tx_bytes = dev->stats.tx_bytes;
	stats->rx_bytes = dev->stats.rx_bytes;
}

static const struct net_device_ops test_netdev_ops = {
	.ndo_get_stats64 = test_ndo_get_stats64,
};

static void test_netdev_setup(struct net_device *dev)
{
	dev->netdev_ops = &test_netdev_ops;
	dev->type       = ARPHRD_NONE;
	dev->flags      = IFF_NOARP;
	dev->needs_free_netdev = true;
}

/* Interruptible sleep; returns non-zero if we should exit now. */
static int sleep_or_stop(u32 ms)
{
	if (msleep_interruptible(ms))
		return 1;
	return kthread_should_stop();
}

/* Inject @bps of tx bytes over @duration_ms in 50ms chunks. */
static void drive_tx_for(struct net_device *dev, u64 bps, u32 duration_ms)
{
	u64 bytes_per_chunk = (bps / 8UL) * 50UL / 1000UL;
	u32 chunks = duration_ms / 50;

	pr_info("drive %s tx=%llu Mbps for %u ms\n",
		dev->name, bps / 1000000ULL, duration_ms);
	while (chunks--) {
		dev->stats.tx_bytes += bytes_per_chunk;
		if (sleep_or_stop(50))
			return;
	}
}

static int scenario_fn(void *unused)
{
	pr_info("=== scenario runner start ===\n");

	/* Prime BHM's netdev baseline. */
	if (sleep_or_stop(500))
		goto done;

	/* SCENARIO 1 */
	pr_info("=== SC1: hysteresis on bhmtest0 ===\n");
	drive_tx_for(tdev0, 150ULL * 1000000ULL, 3000);
	if (kthread_should_stop())
		goto done;
	drive_tx_for(tdev0, 600ULL * 1000000ULL, 3000);
	if (kthread_should_stop())
		goto done;
	pr_info("SC1: idle (wait for fall to L0)\n");
	if (sleep_or_stop(2000))
		goto done;

	/* SCENARIO 2 */
	pr_info("=== SC2: per-BH filter — traffic on bhmtest1 only ===\n");
	pr_info("SC2: bh_global should react; bh_wlan0 should stay at L0\n");
	drive_tx_for(tdev1, 200ULL * 1000000ULL, 3000);
	if (kthread_should_stop())
		goto done;
	pr_info("SC2: idle\n");
	if (sleep_or_stop(2000))
		goto done;

	/* SCENARIO 3 */
	pr_info("=== SC3: override net-change filter ===\n");
	pr_info("SC3: fast toggle (force→clear back-to-back); expect NO cb\n");
	bhm_force_override(bh_override);
	bhm_clear_override(bh_override);
	if (sleep_or_stop(500))
		goto done;

	pr_info("SC3: stable force; expect cb[override] ov=1\n");
	bhm_force_override(bh_override);
	if (sleep_or_stop(500))
		goto done;

	pr_info("SC3: stable clear; expect cb[override] ov=0\n");
	bhm_clear_override(bh_override);
	if (sleep_or_stop(500))
		goto done;

	/* SCENARIO 4 */
	pr_info("=== SC4: bhm_work_to_bh accessor ===\n");
	pr_info("SC4: bhm_schedule triggers user work fn; expect accessor_fn log\n");
	bhm_schedule(bh_accessor);
	if (sleep_or_stop(500))
		goto done;

	pr_info("=== all scenarios done; idling until rmmod ===\n");
	while (!sleep_or_stop(500))
		;
done:
	return 0;
}

static struct bhm_bh *register_bh_wq(const struct bhm_level *lv, u32 nr,
				      const char * const *filter,
				      void (*user_fn)(struct work_struct *))
{
	struct bhm_params p;

	memset(&p, 0, sizeof(p));
	p.type            = BHM_TYPE_WORKQUEUE;
	p.u.work.fn       = user_fn;
	p.u.work.wq       = NULL;
	p.levels          = lv;
	p.nr_levels       = nr;
	p.hyst.rise_ticks = 1;
	p.hyst.fall_ticks = 10;
	p.netdev_names    = filter;
	return bhm_register(&p, NULL);
}

static int __init bhm_test_init(void)
{
	int ret;

	pr_info("loading\n");
	test_start = ktime_get();

	tdev0 = alloc_netdev(0, "bhmtest0", NET_NAME_USER, test_netdev_setup);
	tdev1 = alloc_netdev(0, "bhmtest1", NET_NAME_USER, test_netdev_setup);
	if (!tdev0 || !tdev1) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	ret = bhm_init();
	if (ret) {
		pr_err("bhm_init failed: %d\n", ret);
		goto err_init;
	}

	ret = bhm_register_netdev(tdev0);
	if (ret)
		goto err_reg_netdev;
	ret = bhm_register_netdev(tdev1);
	if (ret)
		goto err_reg_netdev1;

	bh_global = register_bh_wq(levels_global, ARRAY_SIZE(levels_global),
				   NULL, noop_work_fn);
	if (IS_ERR(bh_global)) {
		ret = PTR_ERR(bh_global);
		bh_global = NULL;
		goto err_reg_bh;
	}

	bh_wlan0 = register_bh_wq(levels_wlan0, ARRAY_SIZE(levels_wlan0),
				   filter_wlan0, noop_work_fn);
	if (IS_ERR(bh_wlan0)) {
		ret = PTR_ERR(bh_wlan0);
		bh_wlan0 = NULL;
		goto err_reg_bh;
	}

	bh_override = register_bh_wq(levels_single_override,
				      ARRAY_SIZE(levels_single_override),
				      NULL, noop_work_fn);
	if (IS_ERR(bh_override)) {
		ret = PTR_ERR(bh_override);
		bh_override = NULL;
		goto err_reg_bh;
	}

	bh_accessor = register_bh_wq(levels_single_accessor,
				      ARRAY_SIZE(levels_single_accessor),
				      NULL, accessor_work_fn);
	if (IS_ERR(bh_accessor)) {
		ret = PTR_ERR(bh_accessor);
		bh_accessor = NULL;
		goto err_reg_bh;
	}

	scenario_task = kthread_run(scenario_fn, NULL, "bhm_scenario");
	if (IS_ERR(scenario_task)) {
		ret = PTR_ERR(scenario_task);
		scenario_task = NULL;
		goto err_reg_bh;
	}

	pr_info("loaded (4 BHs, 2 netdevs)\n");
	return 0;

err_reg_bh:
	if (bh_accessor) bhm_unregister(bh_accessor);
	if (bh_override) bhm_unregister(bh_override);
	if (bh_wlan0)    bhm_unregister(bh_wlan0);
	if (bh_global)   bhm_unregister(bh_global);
	bhm_unregister_netdev(tdev1);
err_reg_netdev1:
	bhm_unregister_netdev(tdev0);
err_reg_netdev:
	bhm_deinit();
err_init:
err_alloc:
	if (tdev1) free_netdev(tdev1);
	if (tdev0) free_netdev(tdev0);
	return ret;
}

static void __exit bhm_test_exit(void)
{
	if (scenario_task) {
		kthread_stop(scenario_task);
		scenario_task = NULL;
	}
	if (bh_accessor) bhm_unregister(bh_accessor);
	if (bh_override) bhm_unregister(bh_override);
	if (bh_wlan0)    bhm_unregister(bh_wlan0);
	if (bh_global)   bhm_unregister(bh_global);
	bhm_unregister_netdev(tdev1);
	bhm_unregister_netdev(tdev0);
	bhm_deinit();
	if (tdev1) free_netdev(tdev1);
	if (tdev0) free_netdev(tdev0);
	pr_info("unloaded\n");
}

module_init(bhm_test_init);
module_exit(bhm_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dujeong Lee");
MODULE_DESCRIPTION("bh_manager test harness");

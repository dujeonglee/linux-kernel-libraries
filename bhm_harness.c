// SPDX-License-Identifier: GPL-2.0
/*
 * bh_manager test harness.
 *
 *   Creates fake netdevs whose byte counters the harness drives, and
 *   registers several BHs that exercise different framework features.
 *   Logs every callback invocation with a millisecond timestamp and a
 *   tag identifying the firing BH.
 *
 *   SC1 — hysteresis on bh_global (bhmtest0 tput 0→150→600→0).
 *   SC2 — per-BH netdev filter: bh_wlan0 (filter=[bhmtest0]) stays at
 *         L0 while bh_global reacts to bhmtest1 traffic.
 *   SC3 — override net-change filter: rapid force→clear collapses;
 *         stable force/clear each fire once.
 *   SC4 — bhm_work_to_bh() accessor.
 *   SC5 — threaded NAPI: inject traffic on a registered, UP netdev
 *         (bhmnapi) and observe level transitions on a
 *         BHM_TYPE_THREADED_NAPI-backed BH.
 *   SC6 — CPU affinity: sweep bhm_set_preferred_cpu over online CPUs
 *         and verify the user work fn runs on each.
 */

#define pr_fmt(fmt) "bhm_test: " fmt

#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/if_arp.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/version.h>

#include "bh_manager.h"

#define L1_BPS (100U * 1000000U)
#define L2_BPS (500U * 1000000U)

static struct net_device *tdev0, *tdev1, *tdev_napi;

static struct bhm_bh *bh_global, *bh_wlan0, *bh_override, *bh_accessor;
static struct bhm_bh *bh_tnapi;
static struct task_struct *scenario_task;
static ktime_t test_start;

static const char * const filter_wlan0[] = { "bhmtest0", NULL };
static const char * const filter_napi[]  = { "bhmnapi", NULL };

static s64 now_ms(void)
{
	return ktime_ms_delta(ktime_get(), test_start);
}

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

static void accessor_work_fn(struct work_struct *w)
{
	struct bhm_bh *bh = bhm_work_to_bh(w);

	pr_info("accessor_fn: t=%lldms bh=%p current_level=%u cpu=%d\n",
		now_ms(), bh, bhm_current_level(bh), smp_processor_id());
}

static int test_napi_poll(struct napi_struct *napi, int budget)
{
	struct bhm_bh *bh = bhm_napi_to_bh(napi);

	pr_info("napi_poll: t=%lldms bh=%p budget=%d cpu=%d\n",
		now_ms(), bh, budget, smp_processor_id());
	napi_complete_done(napi, 0);
	return 0;
}

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

static const struct bhm_level levels_tnapi[] = {
	{ .threshold_bps = 0,      .dir = BHM_DIR_BOTH, .cb = cb_common, .ctx = "tnapi" },
	{ .threshold_bps = L1_BPS, .dir = BHM_DIR_BOTH, .cb = cb_common, .ctx = "tnapi" },
	{ .threshold_bps = L2_BPS, .dir = BHM_DIR_BOTH, .cb = cb_common, .ctx = "tnapi" },
};

static const struct bhm_level levels_single_override[] = {
	{ .threshold_bps = 0, .dir = BHM_DIR_BOTH, .cb = cb_common, .ctx = "override" },
};

static const struct bhm_level levels_single_accessor[] = {
	{ .threshold_bps = 0, .dir = BHM_DIR_BOTH, .cb = cb_common, .ctx = "access" },
};

/* ---- Fake netdev plumbing -------------------------------------------- */

static void test_ndo_get_stats64(struct net_device *dev,
				  struct rtnl_link_stats64 *stats)
{
	stats->tx_bytes = dev->stats.tx_bytes;
	stats->rx_bytes = dev->stats.rx_bytes;
}

/* Minimal (unregistered) netdev used for bhmtest0/1. */
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

/* ---- Scenario driver -------------------------------------------------- */

static int sleep_or_stop(u32 ms)
{
	if (msleep_interruptible(ms))
		return 1;
	return kthread_should_stop();
}

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
	unsigned int cpu;
	unsigned int cpus_tested = 0;

	pr_info("=== scenario runner start ===\n");

	if (sleep_or_stop(500))
		goto done;

	/* SC1 */
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

	/* SC2 */
	pr_info("=== SC2: per-BH filter — traffic on bhmtest1 only ===\n");
	pr_info("SC2: bh_global should react; bh_wlan0 should stay at L0\n");
	drive_tx_for(tdev1, 200ULL * 1000000ULL, 3000);
	if (kthread_should_stop())
		goto done;
	pr_info("SC2: idle\n");
	if (sleep_or_stop(2000))
		goto done;

	/* SC3 */
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

	/* SC4 */
	pr_info("=== SC4: bhm_work_to_bh accessor ===\n");
	pr_info("SC4: bhm_schedule triggers user work fn; expect accessor_fn log\n");
	bhm_schedule(bh_accessor);
	if (sleep_or_stop(500))
		goto done;

	/* SC5 */
	if (bh_tnapi) {
		pr_info("=== SC5: threaded NAPI on bhmnapi ===\n");
		drive_tx_for(tdev_napi, 200ULL * 1000000ULL, 3000);
		if (kthread_should_stop())
			goto done;
		pr_info("SC5: idle\n");
		if (sleep_or_stop(2000))
			goto done;
	} else {
		pr_info("=== SC5: skipped (threaded NAPI not available) ===\n");
	}

	/* SC6 */
	pr_info("=== SC6: CPU affinity (bh_accessor) ===\n");
	for_each_online_cpu(cpu) {
		if (cpus_tested >= 4)
			break;
		pr_info("SC6: preferred cpu=%u\n", cpu);
		bhm_set_preferred_cpu(bh_accessor, (int)cpu);
		bhm_schedule(bh_accessor);
		cpus_tested++;
		if (sleep_or_stop(200))
			goto done;
	}
	bhm_set_preferred_cpu(bh_accessor, -1);
	pr_info("SC6: cleared preference\n");

	pr_info("=== all scenarios done; idling until rmmod ===\n");
	while (!sleep_or_stop(500))
		;
done:
	return 0;
}

/* ---- Registration helpers -------------------------------------------- */

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

static struct bhm_bh *register_bh_tnapi(void)
{
	struct bhm_params p;

	memset(&p, 0, sizeof(p));
	p.type            = BHM_TYPE_THREADED_NAPI;
	p.u.napi.poll     = test_napi_poll;
	p.u.napi.weight   = 64;
	p.u.napi.dev      = tdev_napi;
	p.levels          = levels_tnapi;
	p.nr_levels       = ARRAY_SIZE(levels_tnapi);
	p.hyst.rise_ticks = 1;
	p.hyst.fall_ticks = 10;
	p.netdev_names    = filter_napi;
	return bhm_register(&p, NULL);
}

/* Use alloc_netdev_dummy(): its internal init sets __LINK_STATE_START
 * so netif_running() is already true — required by dev_set_threaded()
 * on v6.16+. Avoids the full register_netdev + dev_open lifecycle and
 * its kernel notifier fan-out.
 */
static int setup_tnapi_netdev(void)
{
#if KERNEL_VERSION(6, 11, 0) <= LINUX_VERSION_CODE
	tdev_napi = alloc_netdev_dummy(0);
	if (!tdev_napi) {
		pr_warn("alloc_netdev_dummy failed — SC5 disabled\n");
		return -ENOMEM;
	}
	/* Rename so our per-BH filter matches a stable name, and install
	 * our stats op so BHM's sampler reads dev->stats (which we drive).
	 */
	strscpy(tdev_napi->name, "bhmnapi", IFNAMSIZ);
	tdev_napi->netdev_ops = &test_netdev_ops;
	return 0;
#else
	pr_info("SC5 disabled (alloc_netdev_dummy requires v6.11+)\n");
	return -ENOSYS;
#endif
}

static void teardown_tnapi_netdev(void)
{
	if (tdev_napi) {
		free_netdev(tdev_napi);
		tdev_napi = NULL;
	}
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

	/* Best-effort: if register/open fails, SC5 simply won't run. */
	setup_tnapi_netdev();

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
	if (tdev_napi) {
		ret = bhm_register_netdev(tdev_napi);
		if (ret)
			goto err_reg_netdev2;
	}

	bh_global = register_bh_wq(levels_global, ARRAY_SIZE(levels_global),
				   NULL, noop_work_fn);
	if (IS_ERR(bh_global)) { ret = PTR_ERR(bh_global); bh_global = NULL; goto err_reg_bh; }

	bh_wlan0 = register_bh_wq(levels_wlan0, ARRAY_SIZE(levels_wlan0),
				   filter_wlan0, noop_work_fn);
	if (IS_ERR(bh_wlan0)) { ret = PTR_ERR(bh_wlan0); bh_wlan0 = NULL; goto err_reg_bh; }

	bh_override = register_bh_wq(levels_single_override,
				      ARRAY_SIZE(levels_single_override),
				      NULL, noop_work_fn);
	if (IS_ERR(bh_override)) { ret = PTR_ERR(bh_override); bh_override = NULL; goto err_reg_bh; }

	bh_accessor = register_bh_wq(levels_single_accessor,
				      ARRAY_SIZE(levels_single_accessor),
				      NULL, accessor_work_fn);
	if (IS_ERR(bh_accessor)) { ret = PTR_ERR(bh_accessor); bh_accessor = NULL; goto err_reg_bh; }

	if (tdev_napi) {
		bh_tnapi = register_bh_tnapi();
		if (IS_ERR(bh_tnapi)) {
			pr_warn("threaded NAPI register failed: %ld — SC5 disabled\n",
				PTR_ERR(bh_tnapi));
			bh_tnapi = NULL;
		}
	}

	scenario_task = kthread_run(scenario_fn, NULL, "bhm_scenario");
	if (IS_ERR(scenario_task)) {
		ret = PTR_ERR(scenario_task);
		scenario_task = NULL;
		goto err_kthread;
	}

	pr_info("loaded (%d BHs, %d netdevs)\n",
		4 + (bh_tnapi ? 1 : 0), 2 + (tdev_napi ? 1 : 0));
	return 0;

err_kthread:
	if (bh_tnapi)    bhm_unregister(bh_tnapi);
err_reg_bh:
	if (bh_accessor) bhm_unregister(bh_accessor);
	if (bh_override) bhm_unregister(bh_override);
	if (bh_wlan0)    bhm_unregister(bh_wlan0);
	if (bh_global)   bhm_unregister(bh_global);
	if (tdev_napi)   bhm_unregister_netdev(tdev_napi);
err_reg_netdev2:
	bhm_unregister_netdev(tdev1);
err_reg_netdev1:
	bhm_unregister_netdev(tdev0);
err_reg_netdev:
	bhm_deinit();
err_init:
	teardown_tnapi_netdev();
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
	if (bh_tnapi)    bhm_unregister(bh_tnapi);
	if (bh_accessor) bhm_unregister(bh_accessor);
	if (bh_override) bhm_unregister(bh_override);
	if (bh_wlan0)    bhm_unregister(bh_wlan0);
	if (bh_global)   bhm_unregister(bh_global);
	if (tdev_napi)   bhm_unregister_netdev(tdev_napi);
	bhm_unregister_netdev(tdev1);
	bhm_unregister_netdev(tdev0);
	bhm_deinit();
	teardown_tnapi_netdev();
	if (tdev1) free_netdev(tdev1);
	if (tdev0) free_netdev(tdev0);
	pr_info("unloaded\n");
}

module_init(bhm_test_init);
module_exit(bhm_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dujeong Lee");
MODULE_DESCRIPTION("bh_manager test harness");

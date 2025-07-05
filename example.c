#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include "kernel_watchdog.h"

static struct watchdog_item *watchdog_item1, *watchdog_item2;

/* Recovery function example */
static void my_recovery_func(void *data)
{
    int *value = (int *)data;
    static int call_count = 0;
    pr_info("Watchdog timeout! Recovery called (count: %d) with data: %d\n",
            ++call_count, *value);
}

/* Module initialization */
static int __init watchdog_example_init(void)
{
    int ret;
    static int recovery_data1 = 100;
    static int recovery_data2 = 200;
    static int recovery_data3 = 300;
    struct watchdog_item *watchdog_item3;

    pr_info("Watchdog example module loaded\n");

    /* Initialize watchdog system (no period needed) */
    ret = watchdog_init();
    if (ret < 0) {
        pr_err("Failed to initialize watchdog: %d\n", ret);
        return ret;
    }

    /* Add watchdog items with different timeouts to test adaptive period */
    watchdog_item1 = watchdog_add(500, my_recovery_func, &recovery_data1);
    if (!watchdog_item1) {
        pr_err("Failed to add watchdog 1\n");
        watchdog_deinit();
        return -ENOMEM;
    }

    watchdog_item2 = watchdog_add(1000, my_recovery_func, &recovery_data2);
    if (!watchdog_item2) {
        pr_err("Failed to add watchdog 2\n");
        watchdog_remove(watchdog_item1);
        watchdog_deinit();
        return -ENOMEM;
    }

    /* Add a short timeout watchdog to trigger period adjustment */
    watchdog_item3 = watchdog_add(200, my_recovery_func, &recovery_data3);
    if (!watchdog_item3) {
        pr_err("Failed to add watchdog 3\n");
        watchdog_remove(watchdog_item1);
        watchdog_remove(watchdog_item2);
        watchdog_deinit();
        return -ENOMEM;
    }

    /* Start watchdogs */
    ret = watchdog_start(watchdog_item1);
    if (ret < 0) {
        pr_err("Failed to start watchdog 1: %d\n", ret);
    }

    ret = watchdog_start(watchdog_item2);
    if (ret < 0) {
        pr_err("Failed to start watchdog 2: %d\n", ret);
    }

    ret = watchdog_start(watchdog_item3);
    if (ret < 0) {
        pr_err("Failed to start watchdog 3: %d\n", ret);
    }

    /* Cancel short timeout watchdog after some time to test period readjustment */
    msleep(200);
    watchdog_remove(watchdog_item3);
    pr_info("Removed short timeout watchdog - period should readjust\n");

    /* Cancel first watchdog after 1200ms */
    msleep(1000);
    watchdog_cancel(watchdog_item1);
    pr_info("Cancelled watchdog 1 after 1200ms\n");

    return 0;
}

/* Module cleanup */
static void __exit watchdog_example_exit(void)
{
    pr_info("Watchdog example module unloaded\n");
    watchdog_deinit();
}

module_init(watchdog_example_init);
module_exit(watchdog_example_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dujeong Lee <dujeong.lee82@gmail.com>");
MODULE_DESCRIPTION("Watchdog Library Example");
MODULE_VERSION("1.0");
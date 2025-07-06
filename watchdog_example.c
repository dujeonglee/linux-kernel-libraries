// SPDX-License-Identifier: GPL-2.0
/*
 * Watchdog Library Example - Linux Kernel Module
 *
 * This module demonstrates the usage of the Linux Kernel Watchdog Library
 * by creating multiple watchdog items with different timeout values and
 * showing how the adaptive period adjustment works.
 *
 * Copyright (C) 2025 Dujeong Lee <dujeong.lee82@gmail.com>
 * Author: Dujeong Lee <dujeong.lee82@gmail.com>
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include "kernel_watchdog.h"

/* Global watchdog item pointers */
static struct watchdog_item *watchdog_item1, *watchdog_item2;

/**
 * my_recovery_func() - Watchdog timeout recovery callback function
 * @data: Private data passed during watchdog registration
 *
 * This function is called when a watchdog timeout occurs. It demonstrates
 * how to implement a recovery function that can access private data and
 * perform recovery actions.
 *
 * Context: Atomic context (called from timer interrupt)
 */
static void my_recovery_func(void *data)
{
	int *value = (int *)data;
	static int call_count = 0;
	
	pr_info("Watchdog timeout! Recovery called (count: %d) with data: %d\n",
		++call_count, *value);
}

/**
 * watchdog_example_init() - Module initialization function
 *
 * Initializes the watchdog system and creates multiple watchdog items
 * with different timeout values to demonstrate:
 * - Watchdog system initialization
 * - Adding watchdog items with different timeouts
 * - Adaptive period adjustment based on shortest timeout
 * - Starting and managing multiple watchdogs
 * - Dynamic removal and period readjustment
 *
 * The function creates three watchdog items:
 * - Item 1: 500ms timeout
 * - Item 2: 1000ms timeout  
 * - Item 3: 200ms timeout (removed after 200ms to test period readjustment)
 *
 * Return: 0 on success, negative error code on failure
 */
static int __init watchdog_example_init(void)
{
	int ret;
	static int recovery_data1 = 100;
	static int recovery_data2 = 200;
	static int recovery_data3 = 300;
	struct watchdog_item *watchdog_item3;

	pr_info("Watchdog example module loaded\n");

	/* Initialize watchdog system */
	ret = watchdog_init();
	if (ret < 0) {
		pr_err("Failed to initialize watchdog: %d\n", ret);
		return ret;
	}

	/*
	 * Add watchdog items with different timeouts to test adaptive period.
	 * The system will automatically adjust the timer period based on the
	 * shortest timeout among active watchdogs.
	 */
	
	/* Add 500ms timeout watchdog */
	watchdog_item1 = watchdog_add(500, my_recovery_func, &recovery_data1);
	if (!watchdog_item1) {
		pr_err("Failed to add watchdog 1\n");
		watchdog_deinit();
		return -ENOMEM;
	}

	/* Add 1000ms timeout watchdog */
	watchdog_item2 = watchdog_add(1000, my_recovery_func, &recovery_data2);
	if (!watchdog_item2) {
		pr_err("Failed to add watchdog 2\n");
		watchdog_remove(watchdog_item1);
		watchdog_deinit();
		return -ENOMEM;
	}

	/*
	 * Add a short timeout watchdog to trigger period adjustment.
	 * This will cause the timer period to be set to 200ms/4 = 50ms.
	 */
	watchdog_item3 = watchdog_add(200, my_recovery_func, &recovery_data3);
	if (!watchdog_item3) {
		pr_err("Failed to add watchdog 3\n");
		watchdog_remove(watchdog_item1);
		watchdog_remove(watchdog_item2);
		watchdog_deinit();
		return -ENOMEM;
	}

	/* Start all watchdogs */
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

	/*
	 * Remove short timeout watchdog after 200ms to test period readjustment.
	 * After removal, the timer period should readjust to the next shortest
	 * timeout (500ms/4 = 125ms).
	 */
	msleep(200);
	watchdog_remove(watchdog_item3);
	pr_info("Removed short timeout watchdog - period should readjust\n");

	/*
	 * Cancel first watchdog after 1200ms total.
	 * This demonstrates canceling a watchdog before timeout occurs.
	 */
	msleep(1000);
	watchdog_cancel(watchdog_item1);
	pr_info("Cancelled watchdog 1 after 1200ms\n");

	return 0;
}

/**
 * watchdog_example_exit() - Module cleanup function
 *
 * Performs cleanup when the module is unloaded. The watchdog_deinit()
 * function will automatically clean up all remaining watchdog items
 * and stop the timer.
 */
static void __exit watchdog_example_exit(void)
{
	pr_info("Watchdog example module unloaded\n");
	
	/*
	 * Deinitialize watchdog system.
	 * This will automatically remove all remaining watchdog items
	 * and stop the timer.
	 */
	watchdog_deinit();
}

module_init(watchdog_example_init);
module_exit(watchdog_example_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dujeong Lee <dujeong.lee82@gmail.com>");
MODULE_DESCRIPTION("Watchdog Library Example");
MODULE_VERSION("1.0");
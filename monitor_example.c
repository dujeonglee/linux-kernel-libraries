// SPDX-License-Identifier: GPL-2.0
/*
 * Monitor Library Example - Linux Kernel Module
 *
 * This module demonstrates the usage of the Linux Kernel Monitor Library
 * by implementing three example monitors: CPU usage, memory usage, and
 * temperature monitoring with simulated data.
 *
 * Copyright (C) 2025 Dujeong Lee <dujeong.lee82@gmail.com>
 * Author: Dujeong Lee <dujeong.lee82@gmail.com>
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include "monitor.h"

/* Global variables */
static struct monitor_manager g_monitor_mgr;
static struct proc_dir_entry *proc_entry;

/* Monitor item pointers */
static struct monitor_item *cpu_item;
static struct monitor_item *memory_item;
static struct monitor_item *temp_item;

/**
 * struct example_data - Private data structure for monitor examples
 * @name: Human-readable name for the monitor
 * @threshold: Threshold value for triggering actions
 * @counter: Counter for tracking monitor function calls
 */
struct example_data {
	char name[32];
	unsigned long threshold;
	unsigned long counter;
};

/**
 * cpu_usage_monitor() - CPU usage monitoring function
 * @private_data: Pointer to example_data structure
 *
 * Simulates CPU usage monitoring by generating random values.
 * In a real implementation, this would read actual CPU usage statistics.
 *
 * Return: CPU usage percentage (0-99)
 */
static unsigned long cpu_usage_monitor(void *private_data)
{
	struct example_data *data = (struct example_data *)private_data;
	unsigned long usage;
	
	/*
	 * Generate random CPU usage value (0-99%)
	 * TODO: Replace with actual CPU usage reading
	 */
	get_random_bytes(&usage, sizeof(usage));
	usage = usage % 100;
	
	data->counter++;
	
	monitor_debug("CPU usage monitor: %lu%% (counter: %lu)", usage, data->counter);
	return usage;
}

/**
 * cpu_usage_action() - CPU usage action callback
 * @old_state: Previous CPU usage value
 * @new_state: Current CPU usage value
 * @private_data: Pointer to example_data structure
 *
 * Called when CPU usage state changes. Logs alerts when usage exceeds
 * the configured threshold.
 */
static void cpu_usage_action(unsigned long old_state, unsigned long new_state, void *private_data)
{
	struct example_data *data = (struct example_data *)private_data;
	
	if (new_state > data->threshold) {
		monitor_info("CPU usage alert: %s - Usage increased from %lu%% to %lu%% (threshold: %lu%%)",
			     data->name, old_state, new_state, data->threshold);
	} else {
		monitor_info("CPU usage normal: %s - Usage decreased from %lu%% to %lu%%",
			     data->name, old_state, new_state);
	}
}

/**
 * memory_usage_monitor() - Memory usage monitoring function
 * @private_data: Pointer to example_data structure
 *
 * Simulates memory usage monitoring by generating random values.
 * In a real implementation, this would read actual memory statistics.
 *
 * Return: Memory usage in MB (0-1023)
 */
static unsigned long memory_usage_monitor(void *private_data)
{
	struct example_data *data = (struct example_data *)private_data;
	unsigned long usage;
	
	/*
	 * Generate random memory usage value (0-1023 MB)
	 * TODO: Replace with actual memory usage reading
	 */
	get_random_bytes(&usage, sizeof(usage));
	usage = usage % 1024;
	
	data->counter++;
	
	monitor_debug("Memory usage monitor: %lu MB (counter: %lu)", usage, data->counter);
	return usage;
}

/**
 * memory_usage_action() - Memory usage action callback
 * @old_state: Previous memory usage value
 * @new_state: Current memory usage value
 * @private_data: Pointer to example_data structure
 *
 * Called when memory usage state changes. Logs alerts when usage exceeds
 * the configured threshold.
 */
static void memory_usage_action(unsigned long old_state, unsigned long new_state, void *private_data)
{
	struct example_data *data = (struct example_data *)private_data;
	
	if (new_state > data->threshold) {
		monitor_info("Memory usage alert: %s - Usage increased from %lu MB to %lu MB (threshold: %lu MB)",
			     data->name, old_state, new_state, data->threshold);
	} else {
		monitor_info("Memory usage normal: %s - Usage decreased from %lu MB to %lu MB",
			     data->name, old_state, new_state);
	}
}

/**
 * temperature_monitor() - Temperature monitoring function
 * @private_data: Pointer to example_data structure
 *
 * Simulates temperature monitoring by generating random values.
 * In a real implementation, this would read actual temperature sensors.
 *
 * Return: Temperature in Celsius (20-79)
 */
static unsigned long temperature_monitor(void *private_data)
{
	struct example_data *data = (struct example_data *)private_data;
	unsigned long temp;
	
	/*
	 * Generate random temperature value (20-79°C)
	 * TODO: Replace with actual temperature sensor reading
	 */
	get_random_bytes(&temp, sizeof(temp));
	temp = 20 + (temp % 60);
	
	data->counter++;
	
	monitor_debug("Temperature monitor: %lu°C (counter: %lu)", temp, data->counter);
	return temp;
}

/**
 * temperature_action() - Temperature action callback
 * @old_state: Previous temperature value
 * @new_state: Current temperature value
 * @private_data: Pointer to example_data structure
 *
 * Called when temperature state changes. Logs alerts when temperature exceeds
 * the configured threshold.
 */
static void temperature_action(unsigned long old_state, unsigned long new_state, void *private_data)
{
	struct example_data *data = (struct example_data *)private_data;
	
	if (new_state > data->threshold) {
		monitor_info("Temperature alert: %s - Temperature increased from %lu°C to %lu°C (threshold: %lu°C)",
			     data->name, old_state, new_state, data->threshold);
	} else {
		monitor_info("Temperature normal: %s - Temperature decreased from %lu°C to %lu°C",
			     data->name, old_state, new_state);
	}
}

/**
 * monitor_proc_show() - Display monitor status in proc filesystem
 * @m: seq_file for output
 * @v: Unused parameter
 *
 * Shows the current status of the monitor manager and all active monitor items.
 * This includes statistics, configuration, and current states.
 *
 * Return: 0 on success
 */
static int monitor_proc_show(struct seq_file *m, void *v)
{
	unsigned long total_checks, total_actions;
	unsigned int active_items;
	int ret;
	
	seq_printf(m, "Monitor Library Example Status\n");
	seq_printf(m, "==============================\n\n");
	
	ret = monitor_get_manager_stats(&g_monitor_mgr, &total_checks, &total_actions, &active_items);
	if (ret == 0) {
		seq_printf(m, "Manager Status:\n");
		seq_printf(m, "  Running: %s\n", g_monitor_mgr.running ? "Yes" : "No");
		seq_printf(m, "  Base Interval: %lu ms\n", g_monitor_mgr.base_interval_ms);
		seq_printf(m, "  Active Items: %u\n", active_items);
		seq_printf(m, "  Total Checks: %lu\n", total_checks);
		seq_printf(m, "  Total Actions: %lu\n", total_actions);
		seq_printf(m, "\n");
	}
	
	/* Display individual item status */
	seq_printf(m, "Individual Item Status:\n");
	
	if (cpu_item && !IS_ERR(cpu_item)) {
		unsigned long state, checks, actions;
		monitor_get_item_state(cpu_item, &state);
		monitor_get_item_stats(cpu_item, &checks, &actions);
		seq_printf(m, "  CPU Monitor (%p):\n", cpu_item);
		seq_printf(m, "    Name: %s\n", cpu_item->name);
		seq_printf(m, "    Current State: %lu\n", state);
		seq_printf(m, "    Checks: %lu, Actions: %lu\n", checks, actions);
		seq_printf(m, "    Interval: %lu ms, Hysteresis: %lu\n", 
			   cpu_item->interval_ms, cpu_item->hysteresis);
		seq_printf(m, "\n");
	}
	
	if (memory_item && !IS_ERR(memory_item)) {
		unsigned long state, checks, actions;
		monitor_get_item_state(memory_item, &state);
		monitor_get_item_stats(memory_item, &checks, &actions);
		seq_printf(m, "  Memory Monitor (%p):\n", memory_item);
		seq_printf(m, "    Name: %s\n", memory_item->name);
		seq_printf(m, "    Current State: %lu\n", state);
		seq_printf(m, "    Checks: %lu, Actions: %lu\n", checks, actions);
		seq_printf(m, "    Interval: %lu ms, Hysteresis: %lu\n", 
			   memory_item->interval_ms, memory_item->hysteresis);
		seq_printf(m, "\n");
	}
	
	if (temp_item && !IS_ERR(temp_item)) {
		unsigned long state, checks, actions;
		monitor_get_item_state(temp_item, &state);
		monitor_get_item_stats(temp_item, &checks, &actions);
		seq_printf(m, "  Temperature Monitor (%p):\n", temp_item);
		seq_printf(m, "    Name: %s\n", temp_item->name);
		seq_printf(m, "    Current State: %lu\n", state);
		seq_printf(m, "    Checks: %lu, Actions: %lu\n", checks, actions);
		seq_printf(m, "    Interval: %lu ms, Hysteresis: %lu\n", 
			   temp_item->interval_ms, temp_item->hysteresis);
		seq_printf(m, "\n");
	}
	
	seq_printf(m, "Note: Use 'dmesg | grep monitor' to see detailed monitor logs\n");
	
	return 0;
}

/**
 * monitor_proc_open() - Open proc file callback
 * @inode: inode of the proc file
 * @file: file structure
 *
 * Return: 0 on success, negative error code on failure
 */
static int monitor_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, monitor_proc_show, NULL);
}

static const struct proc_ops monitor_proc_ops = {
	.proc_open = monitor_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/**
 * monitor_example_init() - Module initialization function
 *
 * Initializes the monitor manager, creates three example monitor items
 * (CPU usage, memory usage, temperature), creates a proc filesystem entry,
 * and starts the monitoring system.
 *
 * Return: 0 on success, negative error code on failure
 */
static int __init monitor_example_init(void)
{
	int ret;
	
	/* Static example data structures */
	static struct example_data cpu_data = {
		.name = "CPU",
		.threshold = 80,	/* 80% threshold */
		.counter = 0
	};
	
	static struct example_data memory_data = {
		.name = "Memory",
		.threshold = 512,	/* 512MB threshold */
		.counter = 0
	};
	
	static struct example_data temp_data = {
		.name = "Temperature",
		.threshold = 70,	/* 70°C threshold */
		.counter = 0
	};
	
	pr_info("Monitor example module loading...\n");
	
	/* Initialize monitor manager with 2 second base interval */
	ret = monitor_manager_init(&g_monitor_mgr, 2000);
	if (ret) {
		pr_err("Failed to initialize monitor manager: %d\n", ret);
		return ret;
	}
	
	/* Add CPU monitor item */
	struct monitor_item_init cpu_init = {
		.name = "cpu_usage",
		.interval_ms = 2000,	/* 2 second interval (1x base_interval) */
		.hysteresis = 3,	/* 3 consecutive identical values required */
		.monitor_func = cpu_usage_monitor,
		.action_func = cpu_usage_action,
		.private_data = &cpu_data
	};
	
	cpu_item = monitor_add_item(&g_monitor_mgr, &cpu_init);
	if (IS_ERR(cpu_item)) {
		ret = PTR_ERR(cpu_item);
		pr_err("Failed to add CPU monitor item: %d\n", ret);
		goto cleanup_mgr;
	}
	pr_info("Added CPU monitor item at %p\n", cpu_item);
	
	/* Add memory monitor item */
	struct monitor_item_init memory_init = {
		.name = "memory_usage",
		.interval_ms = 4000,	/* 4 second interval (2x base_interval) */
		.hysteresis = 2,	/* 2 consecutive identical values required */
		.monitor_func = memory_usage_monitor,
		.action_func = memory_usage_action,
		.private_data = &memory_data
	};
	
	memory_item = monitor_add_item(&g_monitor_mgr, &memory_init);
	if (IS_ERR(memory_item)) {
		ret = PTR_ERR(memory_item);
		pr_err("Failed to add memory monitor item: %d\n", ret);
		goto cleanup_mgr;
	}
	pr_info("Added memory monitor item at %p\n", memory_item);
	
	/* Add temperature monitor item */
	struct monitor_item_init temp_init = {
		.name = "temperature",
		.interval_ms = 6000,	/* 6 second interval (3x base_interval) */
		.hysteresis = 4,	/* 4 consecutive identical values required */
		.monitor_func = temperature_monitor,
		.action_func = temperature_action,
		.private_data = &temp_data
	};
	
	temp_item = monitor_add_item(&g_monitor_mgr, &temp_init);
	if (IS_ERR(temp_item)) {
		ret = PTR_ERR(temp_item);
		pr_err("Failed to add temperature monitor item: %d\n", ret);
		goto cleanup_mgr;
	}
	pr_info("Added temperature monitor item at %p\n", temp_item);
	
	/* Create proc filesystem entry */
	proc_entry = proc_create("monitor_example", 0444, NULL, &monitor_proc_ops);
	if (!proc_entry) {
		pr_err("Failed to create proc entry\n");
		ret = -ENOMEM;
		goto cleanup_mgr;
	}
	
	/* Start monitoring system */
	ret = monitor_start(&g_monitor_mgr);
	if (ret) {
		pr_err("Failed to start monitor: %d\n", ret);
		goto cleanup_proc;
	}
	
	pr_info("Monitor example module loaded successfully\n");
	pr_info("Check /proc/monitor_example for status\n");
	pr_info("Use 'dmesg | grep monitor' to see monitor logs\n");
	
	return 0;
	
cleanup_proc:
	proc_remove(proc_entry);
cleanup_mgr:
	monitor_manager_cleanup(&g_monitor_mgr);
	return ret;
}

/**
 * monitor_example_exit() - Module cleanup function
 *
 * Stops the monitoring system, removes all monitor items, cleans up
 * the proc filesystem entry, and performs final cleanup of the
 * monitor manager.
 */
static void __exit monitor_example_exit(void)
{
	pr_info("Monitor example module unloading...\n");
	
	/* Stop monitoring system */
	monitor_stop(&g_monitor_mgr);
	
	/* Remove proc filesystem entry */
	if (proc_entry) {
		proc_remove(proc_entry);
	}
	
	/* Remove individual monitor items */
	if (cpu_item && !IS_ERR(cpu_item)) {
		monitor_remove_item(&g_monitor_mgr, cpu_item);
	}
	if (memory_item && !IS_ERR(memory_item)) {
		monitor_remove_item(&g_monitor_mgr, memory_item);
	}
	if (temp_item && !IS_ERR(temp_item)) {
		monitor_remove_item(&g_monitor_mgr, temp_item);
	}
	
	/* Cleanup monitor manager */
	monitor_manager_cleanup(&g_monitor_mgr);
	
	pr_info("Monitor example module unloaded successfully\n");
}

module_init(monitor_example_init);
module_exit(monitor_example_exit);

MODULE_AUTHOR("Dujeong Lee<dujeong.lee82@gmail.com>");
MODULE_DESCRIPTION("Example usage of Linux Kernel Monitor Library");
MODULE_VERSION("1.0.0");
MODULE_LICENSE("GPL");
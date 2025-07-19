// SPDX-License-Identifier: GPL-2.0
/*
 * State Watcher Library Example - Linux Kernel Module
 *
 * This module demonstrates the usage of the Linux Kernel State Watcher Library
 * by implementing three example state watchers: CPU usage, memory usage, and
 * temperature watching with simulated data. Also demonstrates forced
 * state functionality.
 *
 * Copyright (C) 2025 Dujeong Lee <dujeong.lee82@gmail.com>
 * Author: Dujeong Lee <dujeong.lee82@gmail.com>
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include <linux/timer.h>
#include "state_watcher.h"

/* Global variables */
static struct state_watcher g_state_watcher;
static struct proc_dir_entry *proc_entry;
static struct timer_list force_state_demo_timer;

/* Watch item pointers */
static struct watch_item *cpu_item;
static struct watch_item *memory_item;
static struct watch_item *temp_item;

/**
 * struct example_data - Private data structure for state examples
 * @name: Human-readable name for the state watcher
 * @threshold: Threshold value for triggering actions
 * @counter: Counter for tracking state function calls
 */
struct example_data {
	char name[32];
	unsigned long threshold;
	unsigned long counter;
};

/**
 * cpu_usage_state() - CPU usage state function
 * @private_data: Pointer to example_data structure
 *
 * Simulates CPU usage state checking by generating random values.
 * In a real implementation, this would read actual CPU usage statistics.
 *
 * Return: CPU usage percentage (0-99)
 */
static unsigned long cpu_usage_state(void *private_data)
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

	state_watcher_debug("CPU usage state: %lu%% (counter: %lu)", usage, data->counter);
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
		state_watcher_info("CPU usage alert: %s - Usage increased from %lu%% to %lu%% (threshold: %lu%%)",
			           data->name, old_state, new_state, data->threshold);
	} else {
		state_watcher_info("CPU usage normal: %s - Usage decreased from %lu%% to %lu%%",
			           data->name, old_state, new_state);
	}
}

/**
 * memory_usage_state() - Memory usage state function
 * @private_data: Pointer to example_data structure
 *
 * Simulates memory usage state checking by generating random values.
 * In a real implementation, this would read actual memory statistics.
 *
 * Return: Memory usage in MB (0-1023)
 */
static unsigned long memory_usage_state(void *private_data)
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

	state_watcher_debug("Memory usage state: %lu MB (counter: %lu)", usage, data->counter);
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
		state_watcher_info("Memory usage alert: %s - Usage increased from %lu MB to %lu MB (threshold: %lu MB)",
			           data->name, old_state, new_state, data->threshold);
	} else {
		state_watcher_info("Memory usage normal: %s - Usage decreased from %lu MB to %lu MB",
			           data->name, old_state, new_state);
	}
}

/**
 * temperature_state() - Temperature state function
 * @private_data: Pointer to example_data structure
 *
 * Simulates temperature state checking by generating random values.
 * In a real implementation, this would read actual temperature sensors.
 *
 * Return: Temperature in Celsius (20-79)
 */
static unsigned long temperature_state(void *private_data)
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

	state_watcher_debug("Temperature state: %lu°C (counter: %lu)", temp, data->counter);
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
		state_watcher_info("Temperature alert: %s - Temperature increased from %lu°C to %lu°C (threshold: %lu°C)",
			           data->name, old_state, new_state, data->threshold);
	} else {
		state_watcher_info("Temperature normal: %s - Temperature decreased from %lu°C to %lu°C",
			           data->name, old_state, new_state);
	}
}

/**
 * force_state_demo_timer_func() - Timer function to demonstrate forced state
 * @timer: Timer structure
 *
 * This timer function demonstrates the forced state functionality by
 * periodically forcing different states on the temperature watch item.
 */
static void force_state_demo_timer_func(struct timer_list *timer)
{
	static int demo_step = 0;

	if (!temp_item) {
		return;
	}

	switch (demo_step % 4) {
	case 0:
		/* Force high temperature for 10 seconds - action called immediately */
		state_watcher_force_state(temp_item, 85, 10000);
		pr_info("Demo: Forced temperature to 85°C for 10 seconds (immediate action)\n");
		break;
	case 1:
		/* Force low temperature for 8 seconds - action called immediately */
		state_watcher_force_state(temp_item, 25, 8000);
		pr_info("Demo: Forced temperature to 25°C for 8 seconds (immediate action)\n");
		break;
	case 2:
		/* Force critical temperature for 5 seconds - action called immediately */
		state_watcher_force_state(temp_item, 95, 5000);
		pr_info("Demo: Forced temperature to 95°C for 5 seconds (immediate action)\n");
		break;
	case 3:
		/* Clear forced state and let normal state watching resume with hysteresis */
		state_watcher_clear_forced_state(temp_item);
		pr_info("Demo: Cleared forced state, normal state watching with hysteresis resumed\n");
		break;
	}

	demo_step++;

	/* Schedule next demo in 15 seconds */
	mod_timer(&force_state_demo_timer, jiffies + msecs_to_jiffies(15000));
}

/**
 * state_watcher_proc_show() - Display state watcher status in proc filesystem
 * @m: seq_file for output
 * @v: Unused parameter
 *
 * Shows the current status of the state watcher and all active watch items.
 * This includes statistics, configuration, current states, and forced state info.
 *
 * Return: 0 on success
 */
static int state_watcher_proc_show(struct seq_file *m, void *v)
{
	unsigned long total_checks, total_actions;
	unsigned int active_items;
	int ret;

	seq_printf(m, "State Watcher Library Example Status\n");
	seq_printf(m, "====================================\n\n");

	ret = state_watcher_get_stats(&g_state_watcher, &total_checks, &total_actions, &active_items);
	if (ret == 0) {
		seq_printf(m, "Watcher Status:\n");
		seq_printf(m, "  Running: %s\n", g_state_watcher.running ? "Yes" : "No");
		seq_printf(m, "  Base Interval: %lu ms\n", g_state_watcher.base_interval_ms);
		seq_printf(m, "  Active Items: %u\n", active_items);
		seq_printf(m, "  Total Checks: %lu\n", total_checks);
		seq_printf(m, "  Total Actions: %lu\n", total_actions);
		seq_printf(m, "\n");
	}

	/* Display individual item status */
	seq_printf(m, "Individual Watch Item Status:\n");

	if (cpu_item) {
		unsigned long state, checks, actions, remaining_ms;
		bool is_forced;

		state_watcher_get_item_state(cpu_item, &state);
		state_watcher_get_item_stats(cpu_item, &checks, &actions);
		is_forced = state_watcher_is_state_forced(cpu_item, &remaining_ms);

		seq_printf(m, "  CPU Watch Item (%p):\n", cpu_item);
		seq_printf(m, "    Name: %s\n", cpu_item->name);
		seq_printf(m, "    Current State: %lu", state);
		if (is_forced) {
			seq_printf(m, " (FORCED - %lu ms remaining)", remaining_ms);
		}
		seq_printf(m, "\n");
		seq_printf(m, "    Checks: %lu, Actions: %lu\n", checks, actions);
		seq_printf(m, "    Interval: %lu ms, Hysteresis: %lu\n", 
			   cpu_item->interval_ms, cpu_item->hysteresis);
		seq_printf(m, "\n");
	}

	if (memory_item) {
		unsigned long state, checks, actions, remaining_ms;
		bool is_forced;

		state_watcher_get_item_state(memory_item, &state);
		state_watcher_get_item_stats(memory_item, &checks, &actions);
		is_forced = state_watcher_is_state_forced(memory_item, &remaining_ms);

		seq_printf(m, "  Memory Watch Item (%p):\n", memory_item);
		seq_printf(m, "    Name: %s\n", memory_item->name);
		seq_printf(m, "    Current State: %lu", state);
		if (is_forced) {
			seq_printf(m, " (FORCED - %lu ms remaining)", remaining_ms);
		}
		seq_printf(m, "\n");
		seq_printf(m, "    Checks: %lu, Actions: %lu\n", checks, actions);
		seq_printf(m, "    Interval: %lu ms, Hysteresis: %lu\n", 
			   memory_item->interval_ms, memory_item->hysteresis);
		seq_printf(m, "\n");
	}

	if (temp_item) {
		unsigned long state, checks, actions, remaining_ms;
		bool is_forced;

		state_watcher_get_item_state(temp_item, &state);
		state_watcher_get_item_stats(temp_item, &checks, &actions);
		is_forced = state_watcher_is_state_forced(temp_item, &remaining_ms);

		seq_printf(m, "  Temperature Watch Item (%p):\n", temp_item);
		seq_printf(m, "    Name: %s\n", temp_item->name);
		seq_printf(m, "    Current State: %lu", state);
		if (is_forced) {
			seq_printf(m, " (FORCED - %lu ms remaining)", remaining_ms);
		}
		seq_printf(m, "\n");
		seq_printf(m, "    Checks: %lu, Actions: %lu\n", checks, actions);
		seq_printf(m, "    Interval: %lu ms, Hysteresis: %lu\n", 
			   temp_item->interval_ms, temp_item->hysteresis);
		seq_printf(m, "\n");
	}
	
	seq_printf(m, "Note: Use 'dmesg | grep state_watcher' to see detailed state watcher logs\n");
	seq_printf(m, "Note: Temperature watch item has automatic forced state demo running\n");

	return 0;
}

/**
 * state_watcher_proc_open() - Open proc file callback
 * @inode: inode of the proc file
 * @file: file structure
 *
 * Return: 0 on success, negative error code on failure
 */
static int state_watcher_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, state_watcher_proc_show, NULL);
}

static const struct proc_ops state_watcher_proc_ops = {
	.proc_open = state_watcher_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/**
 * state_watcher_example_init() - Module initialization function
 *
 * Initializes the state watcher, creates three example watch items
 * (CPU usage, memory usage, temperature), creates a proc filesystem entry,
 * starts the state watching system, and sets up the forced state demo timer.
 *
 * Return: 0 on success, negative error code on failure
 */
static int __init state_watcher_example_init(void)
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

	pr_info("State watcher example module loading...\n");

	/* Initialize state watcher with 2 second base interval */
	ret = state_watcher_init(&g_state_watcher, 2000);
	if (ret) {
		pr_err("Failed to initialize state watcher: %d\n", ret);
		return ret;
	}

	/* Add CPU watch item */
	struct watch_item_init cpu_init = {
		.name = "cpu_usage",
		.interval_ms = 2000,	/* 2 second interval (1x base_interval) */
		.hysteresis = 3,	/* 3 consecutive identical values required */
		.state_func = cpu_usage_state,
		.action_func = cpu_usage_action,
		.private_data = &cpu_data
	};

	cpu_item = state_watcher_add_item(&g_state_watcher, &cpu_init);
	if (!cpu_item) {
		pr_err("Failed to add CPU watch item\n");
		ret = -ENOMEM;
		goto cleanup_watcher;
	}
	pr_info("Added CPU watch item at %p\n", cpu_item);

	/* Add memory watch item */
	struct watch_item_init memory_init = {
		.name = "memory_usage",
		.interval_ms = 4000,	/* 4 second interval (2x base_interval) */
		.hysteresis = 2,	/* 2 consecutive identical values required */
		.state_func = memory_usage_state,
		.action_func = memory_usage_action,
		.private_data = &memory_data
	};

	memory_item = state_watcher_add_item(&g_state_watcher, &memory_init);
	if (!memory_item) {
		pr_err("Failed to add memory watch item\n");
		ret = -ENOMEM;
		goto cleanup_watcher;
	}
	pr_info("Added memory watch item at %p\n", memory_item);

	/* Add temperature watch item */
	struct watch_item_init temp_init = {
		.name = "temperature",
		.interval_ms = 6000,	/* 6 second interval (3x base_interval) */
		.hysteresis = 4,	/* 4 consecutive identical values required */
		.state_func = temperature_state,
		.action_func = temperature_action,
		.private_data = &temp_data
	};

	temp_item = state_watcher_add_item(&g_state_watcher, &temp_init);
	if (!temp_item) {
		pr_err("Failed to add temperature watch item\n");
		ret = -ENOMEM;
		goto cleanup_watcher;
	}
	pr_info("Added temperature watch item at %p\n", temp_item);

	/* Create proc filesystem entry */
	proc_entry = proc_create("state_watcher_example", 0444, NULL, &state_watcher_proc_ops);
	if (!proc_entry) {
		pr_err("Failed to create proc entry\n");
		ret = -ENOMEM;
		goto cleanup_watcher;
	}

	/* Start state watching system */
	ret = state_watcher_start(&g_state_watcher);
	if (ret) {
		pr_err("Failed to start state watcher: %d\n", ret);
		goto cleanup_proc;
	}

	/* Setup forced state demo timer */
	timer_setup(&force_state_demo_timer, force_state_demo_timer_func, 0);
	mod_timer(&force_state_demo_timer, jiffies + msecs_to_jiffies(10000)); /* Start in 10 seconds */

	pr_info("State watcher example module loaded successfully\n");
	pr_info("Check /proc/state_watcher_example for status\n");
	pr_info("Use 'dmesg | grep state_watcher' to see state watcher logs\n");
	pr_info("Forced state demo will start in 10 seconds\n");

	return 0;

cleanup_proc:
	proc_remove(proc_entry);
cleanup_watcher:
	state_watcher_cleanup(&g_state_watcher);
	return ret;
}

/**
 * state_watcher_example_exit() - Module cleanup function
 *
 * Stops the state watching system, removes all watch items, cleans up
 * the proc filesystem entry, deletes the demo timer, and performs 
 * final cleanup of the state watcher.
 */
static void __exit state_watcher_example_exit(void)
{
	pr_info("State watcher example module unloading...\n");

	/* Stop forced state demo timer */
	del_timer_sync(&force_state_demo_timer);

	/* Stop state watching system */
	state_watcher_stop(&g_state_watcher);

	/* Remove proc filesystem entry */
	if (proc_entry) {
		proc_remove(proc_entry);
	}

	/* Remove individual watch items */
	if (cpu_item) {
		state_watcher_remove_item(&g_state_watcher, cpu_item);
	}
	if (memory_item) {
		state_watcher_remove_item(&g_state_watcher, memory_item);
	}
	if (temp_item) {
		state_watcher_remove_item(&g_state_watcher, temp_item);
	}

	/* Cleanup state watcher */
	state_watcher_cleanup(&g_state_watcher);

	pr_info("State watcher example module unloaded successfully\n");
}

module_init(state_watcher_example_init);
module_exit(state_watcher_example_exit);

MODULE_AUTHOR("Dujeong Lee<dujeong.lee82@gmail.com>");
MODULE_DESCRIPTION("Example usage of Linux Kernel State Watcher Library with forced state demo");
MODULE_VERSION("1.0.0");
MODULE_LICENSE("GPL");
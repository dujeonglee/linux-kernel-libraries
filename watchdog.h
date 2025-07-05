/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Kernel Watchdog Library
 *
 * A high-performance, adaptive watchdog system for Linux kernel drivers.
 * Features lock-free operations, on-demand work scheduling, and automatic
 * period adjustment based on timeout requirements.
 *
 * Copyright (C) 2025 Your Name
 */

#ifndef __KERNEL_WATCHDOG_H__
#define __KERNEL_WATCHDOG_H__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/bug.h>

/*
 * Minimum allowed timeout to prevent excessive CPU usage.
 * Timeouts shorter than this will trigger BUG() to protect system stability.
 */
#define WATCHDOG_MIN_TIMEOUT_MS     100

/*
 * Maximum work period (shortest possible work interval).
 * This ensures the system doesn't become overloaded with too frequent checks.
 */
#define WATCHDOG_MAX_WORK_PERIOD_MS 50

/**
 * struct watchdog_item - Individual watchdog entry
 * @list: List head for linking into the global watchdog list
 * @timeout_ms: Timeout value in milliseconds
 * @start_time: Start time recorded in jiffies when watchdog_start() is called
 * @active: Atomic flag indicating if this watchdog is actively being monitored
 * @recovery_func: Function pointer to call when timeout occurs
 * @private_data: Opaque pointer passed to recovery function
 * @valid: Atomic flag for safe memory management and use-after-free prevention
 *
 * This structure represents a single watchdog timer. The atomic fields allow
 * lock-free operations on the hot paths (start/cancel) while maintaining
 * thread safety.
 */
struct watchdog_item {
	struct list_head list;
	unsigned long timeout_ms;
	unsigned long start_time;
	atomic_t active;                   /* Lock-free active state */
	void (*recovery_func)(void *data);
	void *private_data;
	atomic_t valid;                    /* Lock-free validity flag */
};

/**
 * struct watchdog_context - Global watchdog system context
 * @work: Delayed work structure for periodic timeout checking
 * @item_list: Head of the list containing all watchdog items
 * @lock: Spinlock protecting list operations (add/remove/traverse)
 * @period_ms: Current work execution period in milliseconds
 * @initialized: Flag indicating if the watchdog system is initialized
 * @work_active: Flag indicating if the periodic work is currently scheduled
 *
 * This structure maintains the global state of the watchdog system. The work
 * is scheduled on-demand: started when the first item is added, adjusted when
 * timeouts change, and stopped when all items are removed.
 */
struct watchdog_context {
	struct delayed_work work;
	struct list_head item_list;
	spinlock_t lock;                   /* Protects list operations only */
	unsigned long period_ms;
	bool initialized;
	bool work_active;                  /* On-demand work scheduling */
};

/*
 * Function prototypes
 */

/**
 * watchdog_init - Initialize the watchdog system
 *
 * Initializes the global watchdog context and prepares for watchdog operations.
 * No periodic work is started until the first watchdog item is added.
 *
 * Return: 0 on success, -EBUSY if already initialized
 */
int watchdog_init(void);

/**
 * watchdog_deinit - Deinitialize the watchdog system
 *
 * Stops all periodic work, removes and frees all watchdog items, and resets
 * the system to uninitialized state. Safe to call even if no items exist.
 */
void watchdog_deinit(void);

/**
 * watchdog_add - Add a new watchdog item
 * @timeout_ms: Timeout in milliseconds (must be >= WATCHDOG_MIN_TIMEOUT_MS)
 * @recovery_func: Function to call when timeout occurs (must not be NULL)
 * @private_data: Opaque pointer passed to recovery function (can be NULL)
 *
 * Creates a new watchdog item and adds it to the monitoring system. If this
 * is the first item, starts the periodic work. If the timeout is shorter than
 * existing items, adjusts the work period for better accuracy.
 *
 * The recovery function will be called repeatedly every work period after the
 * timeout occurs, until watchdog_cancel() or watchdog_remove() is called.
 *
 * Return: Pointer to watchdog item on success, NULL on failure or BUG() if
 *         timeout_ms < WATCHDOG_MIN_TIMEOUT_MS
 */
struct watchdog_item *watchdog_add(unsigned long timeout_ms,
				   void (*recovery_func)(void *data),
				   void *private_data);

/**
 * watchdog_remove - Remove and free a watchdog item
 * @item: Watchdog item to remove (must be valid pointer from watchdog_add)
 *
 * Removes the specified watchdog item from monitoring and frees its memory.
 * If this was the last item, stops the periodic work. If other items remain,
 * may adjust the work period based on remaining timeouts.
 *
 * Return: 0 on success, negative error code on failure
 */
int watchdog_remove(struct watchdog_item *item);

/**
 * watchdog_start - Start monitoring a watchdog item (Lock-free)
 * @item: Watchdog item to start (must be valid pointer from watchdog_add)
 *
 * Begins timeout monitoring for the specified item. Records the current time
 * and activates the watchdog. This operation is lock-free for maximum
 * performance on hot paths.
 *
 * Return: 0 on success, negative error code on failure
 */
int watchdog_start(struct watchdog_item *item);

/**
 * watchdog_cancel - Stop monitoring a watchdog item (Lock-free)
 * @item: Watchdog item to cancel (must be valid pointer from watchdog_add)
 *
 * Stops timeout monitoring for the specified item. The recovery function
 * will no longer be called. This operation is lock-free for maximum
 * performance on hot paths.
 *
 * Return: 0 on success, negative error code on failure
 */
int watchdog_cancel(struct watchdog_item *item);

#endif /* __KERNEL_WATCHDOG_H__ */
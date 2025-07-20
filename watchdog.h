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

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/workqueue.h>

/**
* WATCHDOG_MIN_TIMEOUT_MS - Minimum allowed watchdog timeout value
*
* This macro defines the minimum timeout value in milliseconds that can be
* used when creating a watchdog item. Timeouts shorter than this value are
* considered dangerous as they would cause excessive CPU usage and potential
* system instability due to too frequent timeout checking.
*
* Any attempt to create a watchdog with a timeout below this threshold will
* result in a BUG() to protect system stability and force developers to
* redesign their timing requirements.
*
* The value of 200ms provides a reasonable balance between:
* - Allowing sufficiently responsive timeout detection
* - Preventing excessive CPU overhead from frequent work scheduling
* - Maintaining system stability under high watchdog load
*/
#define WATCHDOG_MIN_TIMEOUT_MS     200

/**
 * WATCHDOG_MAX_WORK_PERIOD_MS - Maximum frequency limit for watchdog work execution
 *
 * This macro defines the shortest possible interval (in milliseconds) between
 * watchdog work function executions. It is automatically calculated as half of
 * the minimum allowed timeout to maintain consistency and ensure accurate
 * timeout detection for the shortest possible watchdog timeouts.
 *
 * The work period is normally calculated as half of the shortest active watchdog
 * timeout to ensure accurate timeout detection. This macro serves as the ceiling
 * value when the shortest timeout equals WATCHDOG_MIN_TIMEOUT_MS.
 *
 * By deriving this value from WATCHDOG_MIN_TIMEOUT_MS, we ensure that:
 * - The fastest possible work execution provides 2x oversampling for minimum timeouts
 * - Consistency is maintained between the two timing constraints
 * - Any changes to the minimum timeout automatically adjust the maximum work frequency
 * - The system remains protected from excessive CPU usage
 *
 * For example, with WATCHDOG_MIN_TIMEOUT_MS = 200ms, this results in a maximum
 * work frequency of 100ms, providing adequate detection accuracy while preserving
 * system performance.
 */
#define WATCHDOG_MAX_WORK_PERIOD_MS (WATCHDOG_MIN_TIMEOUT_MS / 2)

/**
* struct watchdog_item - Individual watchdog timer entry
* @list: List head for linking into the global watchdog list
* @timeout_ms: Timeout value in milliseconds, must be >= WATCHDOG_MIN_TIMEOUT_MS
* @start_time: Start time recorded in jiffies when watchdog_start() is called
* @active: Atomic flag indicating if this watchdog is actively being monitored
* @recovery_func: Function pointer to call when timeout occurs
* @private_data: Opaque pointer passed to recovery function, can be NULL
* @valid: Atomic flag for safe memory management and use-after-free prevention
*
* This structure represents a single watchdog timer instance. Each watchdog
* item can be independently started, cancelled, and removed from the monitoring
* system.
*
* The atomic fields (@active and @valid) enable lock-free operations on hot
* paths (start/cancel) while maintaining thread safety. The @valid flag prevents
* use-after-free scenarios when an item is being removed while other threads
* might still hold references to it.
*
* Lifecycle:
* 1. Created via watchdog_add() in inactive state (@active = 0)
* 2. Activated via watchdog_start() which sets @start_time and @active = 1
* 3. Can be deactivated via watchdog_cancel() which sets @active = 0
* 4. Destroyed via watchdog_remove() which sets @valid = 0 and frees memory
*
* The @recovery_func is called repeatedly every work period after timeout
* occurs, until the watchdog is cancelled or removed. This allows for
* continuous recovery attempts rather than one-shot timeout handling.
*
* Example:
* @code
* void my_recovery_func(void *data)
* {
*     struct my_device *dev = (struct my_device *)data;
*     dev_err(&dev->pdev->dev, "Watchdog timeout - resetting device\n");
*     my_device_reset(dev);
* }
*
* struct watchdog_item *wdog;
* wdog = watchdog_add(5000, my_recovery_func, my_device);
* if (wdog) {
*     watchdog_start(wdog);  // Start monitoring with 5 second timeout
*     // ... device operations ...
*     watchdog_cancel(wdog); // Cancel before removing
*     watchdog_remove(wdog); // Clean up
* }
* @endcode
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
* This structure maintains the global state of the watchdog system. There is
* only one instance of this structure per system, managing all watchdog items
* in a centralized manner.
*
* The @work is scheduled on-demand based on the presence of active watchdog
* items. When no items exist, the work is completely stopped to achieve zero
* CPU overhead. The @period_ms is dynamically calculated as half of the
* shortest timeout among all valid items, but clamped to prevent excessive
* CPU usage.
*
* The @lock protects only list structure modifications (add/remove operations
* and list traversal during timeout checking). The hot-path operations
* (start/cancel) are lock-free using atomic operations on individual items.
*
* Work scheduling behavior:
* - @work_active = false: No work scheduled, zero CPU overhead
* - @work_active = true: Work running with current @period_ms interval
* - Period automatically adjusts when items with shorter timeouts are added
* - Work stops automatically when the last item is removed
*
* Example:
* @code
* // System initialization
* watchdog_init();
* 
* // Adding first item starts the work automatically
* struct watchdog_item *wdog1 = watchdog_add(2000, recovery_func1, dev1);
* // period_ms = 1000ms (2000/2), work_active = true
* 
* // Adding shorter timeout adjusts the period
* struct watchdog_item *wdog2 = watchdog_add(500, recovery_func2, dev2);
* // period_ms = 250ms (500/2), work rescheduled with new period
* 
* // Removing all items stops the work
* watchdog_remove(wdog1);
* watchdog_remove(wdog2);
* // work_active = false, period_ms = 0, zero CPU overhead
* @endcode
*/
struct watchdog_context {
   struct delayed_work work;
   struct list_head item_list;
   spinlock_t lock;                   /* Protects list operations only */
   unsigned long period_ms;
   bool initialized;
   bool work_active;                  /* On-demand work scheduling */
};

/**
 * watchdog_init() - Initialize the watchdog system
 */
int watchdog_init(void);

/**
 * watchdog_deinit() - Deinitialize and cleanup the watchdog system
 */
void watchdog_deinit(void);

/**
 * watchdog_add() - Add a new watchdog item with specified timeout and recovery function
 */
struct watchdog_item *watchdog_add(unsigned long timeout_ms,
   			   void (*recovery_func)(void *data),
   			   void *private_data);

/**
 * watchdog_remove() - Remove and free a watchdog item from monitoring
 */
int watchdog_remove(struct watchdog_item *item);

/**
 * watchdog_start() - Start monitoring a watchdog item (lock-free)
 */
int watchdog_start(struct watchdog_item *item);

/**
 * watchdog_cancel() - Stop monitoring a watchdog item (lock-free)
 */
int watchdog_cancel(struct watchdog_item *item);

#endif /* __KERNEL_WATCHDOG_H__ */
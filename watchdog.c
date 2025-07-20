// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel Watchdog Library Implementation
 *
 * A high-performance, adaptive watchdog system for Linux kernel drivers.
 *
 * Key Features:
 * - Lock-free start/cancel operations for hot paths
 * - On-demand work scheduling (zero overhead when idle)
 * - Adaptive period adjustment based on shortest timeout
 * - Continuous recovery function calls after timeout
 * - Thread-safe add/remove operations
 * - Built-in safety limits to prevent system overload
 *
 * Design Philosophy:
 * - Performance: Lock-free operations where possible
 * - Efficiency: Work only runs when watchdogs exist
 * - Safety: Strict timeout limits and error checking
 * - Accuracy: Adaptive periods ensure precise timeout detection
 * - Simplicity: Automatic period management, no user tuning needed
 *
 * Copyright (C) 2025 Dujeong Lee <dujeong.lee82@gmail.com>
 */

#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "kernel_watchdog.h"
/* Global watchdog context - single instance per system */
static struct watchdog_context g_watchdog_ctx;

/* Forward declarations */
static void update_work_period(void);

/**
 * watchdog_work_func - Periodic work function that checks for timeouts
 * @work: Work structure (embedded in watchdog_context)
 *
 * This function runs periodically to check all active watchdog items for
 * timeouts. When a timeout is detected, it calls the associated recovery
 * function and continues to call it every period until the watchdog is
 * cancelled or removed.
 *
 * The function temporarily releases the spinlock when calling recovery
 * functions to avoid holding locks during potentially long-running callbacks.
 * It uses list_for_each_entry_safe to handle potential list modifications
 * during recovery function execution.
 *
 * Context: Workqueue context (sleepable)
 */
static void watchdog_work_func(struct work_struct *work)
{
	struct watchdog_context *ctx = container_of(work, struct watchdog_context, work.work);
	struct watchdog_item *item, *tmp;
	unsigned long flags;
	unsigned long current_time = jiffies;

	spin_lock_irqsave(&ctx->lock, flags);

	/*
	 * Check all valid and active watchdog items for timeouts.
	 * We use atomic reads to avoid races with lock-free start/cancel.
	 */
	list_for_each_entry_safe(item, tmp, &ctx->item_list, list) {
		if (atomic_read(&item->valid) && atomic_read(&item->active)) {
			unsigned long elapsed_ms = jiffies_to_msecs(current_time - item->start_time);

			/* Check if timeout occurred */
			if (elapsed_ms >= item->timeout_ms) {
				/*
				 * Call recovery function every time until cancelled.
				 * Release lock during callback to avoid holding it
				 * during potentially long-running recovery operations.
				 */
				if (item->recovery_func) {
					spin_unlock_irqrestore(&ctx->lock, flags);
					item->recovery_func(item->private_data);
					spin_lock_irqsave(&ctx->lock, flags);
				}
				/*
				 * Keep active=1 so recovery will be called again
				 * next period. Only watchdog_cancel() or 
				 * watchdog_remove() will stop the calls.
				 */
			}
		}
	}

	spin_unlock_irqrestore(&ctx->lock, flags);

	/* Schedule next work iteration if system is still active */
	if (ctx->initialized && ctx->work_active) {
		schedule_delayed_work(&ctx->work, msecs_to_jiffies(ctx->period_ms));
	}
}

/**
* watchdog_init - Initialize the watchdog system
*
* Initializes the global watchdog context and prepares the system for
* watchdog operations. This function must be called before any other
* watchdog operations can be performed.
*
* The initialization sets up the internal data structures but does not
* start any periodic work. The work is started on-demand when the first
* watchdog item is added, providing zero CPU overhead when the system
* is idle.
*
* This function is safe to call multiple times - subsequent calls will
* return -EBUSY without affecting the already initialized system.
*
* Context: Process context
* Return: 0 on success, -EBUSY if already initialized
*
* Example:
* @code
* // Initialize the watchdog system during module init
* static int __init my_module_init(void)
* {
*     int ret = watchdog_init();
*     if (ret) {
*         pr_err("Failed to initialize watchdog system: %d\n", ret);
*         return ret;
*     }
*     
*     // Now safe to use watchdog_add(), watchdog_start(), etc.
*     return 0;
* }
* 
* // Or during device probe
* static int my_device_probe(struct platform_device *pdev)
* {
*     if (watchdog_init() && ret != -EBUSY) {
*         dev_err(&pdev->dev, "Watchdog init failed\n");
*         return -ENODEV;
*     }
*     // Continue with device initialization...
* }
* @endcode
*/
int watchdog_init(void)
{
	if (g_watchdog_ctx.initialized) {
		pr_warn("Watchdog already initialized\n");
		return -EBUSY;
	}

	/* Initialize context to clean state */
	memset(&g_watchdog_ctx, 0, sizeof(g_watchdog_ctx));
	INIT_LIST_HEAD(&g_watchdog_ctx.item_list);
	spin_lock_init(&g_watchdog_ctx.lock);
	g_watchdog_ctx.period_ms = 0;  /* Will be set when first item is added */
	g_watchdog_ctx.initialized = true;
	g_watchdog_ctx.work_active = false;

	/* Initialize delayed work but don't schedule it yet */
	INIT_DELAYED_WORK(&g_watchdog_ctx.work, watchdog_work_func);


	return 0;
}

/**
* watchdog_deinit - Deinitialize the watchdog system
*
* Stops all periodic work, removes and frees all watchdog items, and resets
* the system to uninitialized state. This function performs complete cleanup
* of the watchdog system and should be called during module exit or system
* shutdown.
*
* The function safely handles cleanup even if watchdog items still exist,
* marking them as invalid before freeing to prevent use-after-free issues
* if other code still holds references to the items.
*
* This function is safe to call multiple times and handles cleanup gracefully
* even if no items exist or the system was never initialized.
*
* After calling this function, watchdog_init() must be called again before
* any watchdog operations can be performed.
*
* Context: Process context (may sleep due to work cancellation)
*
* Example:
* @code
* // During module exit
* static void __exit my_module_exit(void)
* {
*     // Clean up any remaining watchdogs and deinitialize
*     watchdog_deinit();
* }
* 
* // During device removal
* static int my_device_remove(struct platform_device *pdev)
* {
*     struct my_device *dev = platform_get_drvdata(pdev);
*     
*     // Cancel device-specific watchdog
*     if (dev->watchdog) {
*         watchdog_cancel(dev->watchdog);
*         watchdog_remove(dev->watchdog);
*     }
*     
*     // If this was the last device using watchdog system
*     watchdog_deinit();
*     
*     return 0;
* }
* @endcode
*/
void watchdog_deinit(void)
{
	struct watchdog_item *item, *tmp;
	unsigned long flags;

	if (!g_watchdog_ctx.initialized) {
		pr_warn("Watchdog not initialized\n");
		return;
	}

	/* Stop the work and prevent further scheduling */
	g_watchdog_ctx.initialized = false;
	g_watchdog_ctx.work_active = false;
	cancel_delayed_work_sync(&g_watchdog_ctx.work);

	/* Remove and free all items */
	spin_lock_irqsave(&g_watchdog_ctx.lock, flags);
	list_for_each_entry_safe(item, tmp, &g_watchdog_ctx.item_list, list) {
		atomic_set(&item->valid, 0);  /* Mark invalid to prevent use */
		list_del(&item->list);
		kfree(item);
	}
	spin_unlock_irqrestore(&g_watchdog_ctx.lock, flags);


}

/**
* watchdog_add - Add a new watchdog item to the monitoring system
* @timeout_ms: Timeout value in milliseconds (must be >= WATCHDOG_MIN_TIMEOUT_MS)
* @recovery_func: Function to call when timeout occurs (must not be NULL)
* @private_data: Opaque pointer passed to recovery function (can be NULL)
*
* Creates a new watchdog item and adds it to the monitoring system. The
* watchdog starts in inactive state; use watchdog_start() to begin monitoring.
* The recovery function will be called repeatedly every work period after the
* timeout occurs, until watchdog_cancel() or watchdog_remove() is called.
*
* If this is the first watchdog item, the periodic work is automatically
* started. If the timeout is shorter than existing items, the work period
* is dynamically adjusted for better accuracy.
*
* The @timeout_ms must be at least WATCHDOG_MIN_TIMEOUT_MS milliseconds to
* prevent excessive CPU usage. Shorter timeouts will trigger BUG() to
* protect system stability.
*
* The @recovery_func should be lightweight and non-blocking as it runs in
* workqueue context with spinlocks temporarily released. It can perform
* recovery actions like device resets, error logging, or state recovery.
*
* Context: Process context
* Return: Pointer to watchdog item on success, NULL on allocation failure,
*         or BUG() if timeout_ms < WATCHDOG_MIN_TIMEOUT_MS
*
* Example:
* @code
* // Recovery function for device timeout
* void device_recovery(void *data)
* {
*     struct my_device *dev = (struct my_device *)data;
*     
*     dev_warn(&dev->pdev->dev, "Device timeout - attempting recovery\n");
*     my_device_reset(dev);
*     
*     // Recovery function is called repeatedly until cancelled
*     if (my_device_is_responsive(dev)) {
*         // Cancel the watchdog once device recovers
*         watchdog_cancel(dev->watchdog);
*     }
* }
* 
* // Add watchdog during device initialization
* static int my_device_probe(struct platform_device *pdev)
* {
*     struct my_device *dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
*     
*     // Add 5-second timeout watchdog
*     dev->watchdog = watchdog_add(5000, device_recovery, dev);
*     if (!dev->watchdog) {
*         dev_err(&pdev->dev, "Failed to create watchdog\n");
*         return -ENOMEM;
*     }
*     
*     // Watchdog is created but not active yet
*     // Use watchdog_start() when operation begins
*     return 0;
* }
* @endcode
*/
struct watchdog_item *watchdog_add(unsigned long timeout_ms,
				   void (*recovery_func)(void *data),
				   void *private_data)
{
	struct watchdog_item *item;
	unsigned long flags;

	if (!g_watchdog_ctx.initialized) {
		pr_err("Watchdog not initialized\n");
		return NULL;
	}

	/* Enforce minimum timeout to protect system stability */
	if (timeout_ms < WATCHDOG_MIN_TIMEOUT_MS) {
		pr_crit("FATAL: Watchdog timeout (%lu ms) is shorter than minimum allowed (%d ms)\n",
			timeout_ms, WATCHDOG_MIN_TIMEOUT_MS);
		pr_crit("This would cause excessive CPU usage and system instability\n");
		pr_crit("Please use timeout >= %d ms or redesign your timing requirements\n",
			WATCHDOG_MIN_TIMEOUT_MS);
		BUG();
	}

	if (!recovery_func) {
		pr_err("Recovery function is NULL\n");
		return NULL;
	}

	/* Allocate new item */
	item = kmalloc(sizeof(*item), GFP_KERNEL);
	if (!item) {
		pr_err("Failed to allocate watchdog item\n");
		return NULL;
	}

	/* Initialize item in inactive state */
	INIT_LIST_HEAD(&item->list);
	item->timeout_ms = timeout_ms;
	item->start_time = 0;
	atomic_set(&item->active, 0);     /* Inactive until watchdog_start() */
	item->recovery_func = recovery_func;
	item->private_data = private_data;
	atomic_set(&item->valid, 1);      /* Valid for use */

	/* Add to global list under lock protection */
	spin_lock_irqsave(&g_watchdog_ctx.lock, flags);
	list_add_tail(&item->list, &g_watchdog_ctx.item_list);
	spin_unlock_irqrestore(&g_watchdog_ctx.lock, flags);

	/* Check if we need to start/adjust work period */
	update_work_period();

	return item;
}

/**
* watchdog_remove - Remove and free a watchdog item from the monitoring system
* @item: Watchdog item to remove (must be valid pointer from watchdog_add)
*
* Removes the specified watchdog item from monitoring and frees its memory.
* The item is marked invalid before removal to prevent use-after-free issues
* if other threads still hold references to it.
*
* If this was the last watchdog item, the periodic work is automatically
* stopped to save CPU resources and achieve zero overhead. If other items
* remain, the work period may be recalculated and adjusted based on the
* remaining shortest timeout.
*
* It is safe to call this function on an active watchdog - it will be
* automatically cancelled during removal. However, it's good practice to
* call watchdog_cancel() explicitly before removal for clarity.
*
* After this function returns, the @item pointer becomes invalid and must
* not be used for any further operations.
*
* Context: Process context (may sleep due to work rescheduling)
* Return: 0 on success, -ENODEV if watchdog system not initialized,
*         -EINVAL if @item is NULL or invalid
*
* Example:
* @code
* // Clean removal during device shutdown
* static int my_device_remove(struct platform_device *pdev)
* {
*     struct my_device *dev = platform_get_drvdata(pdev);
*     
*     if (dev->watchdog) {
*         // Good practice: explicitly cancel before removing
*         watchdog_cancel(dev->watchdog);
*         
*         // Remove and free the watchdog
*         int ret = watchdog_remove(dev->watchdog);
*         if (ret) {
*             dev_warn(&pdev->dev, "Failed to remove watchdog: %d\n", ret);
*         }
*         
*         // Mark as removed to prevent double-free
*         dev->watchdog = NULL;
*     }
*     
*     return 0;
* }
* 
* // Error handling during device initialization
* static int my_device_probe(struct platform_device *pdev)
* {
*     struct my_device *dev;
*     int ret;
*     
*     dev->watchdog = watchdog_add(2000, recovery_func, dev);
*     if (!dev->watchdog)
*         return -ENOMEM;
*     
*     ret = my_device_init_hardware(dev);
*     if (ret) {
*         // Clean up on error
*         watchdog_remove(dev->watchdog);
*         return ret;
*     }
*     
*     return 0;
* }
* @endcode
*/
int watchdog_remove(struct watchdog_item *item)
{
	unsigned long flags;

	if (!g_watchdog_ctx.initialized) {
		pr_err("Watchdog not initialized\n");
		return -ENODEV;
	}

	if (!item) {
		pr_err("Invalid watchdog item pointer\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&g_watchdog_ctx.lock, flags);

	/* Verify item is still valid */
	if (!atomic_read(&item->valid)) {
		spin_unlock_irqrestore(&g_watchdog_ctx.lock, flags);
		pr_err("Watchdog item %p is invalid\n", item);
		return -EINVAL;
	}

	/* Mark invalid first to prevent further use */
	atomic_set(&item->valid, 0);

	/* Remove from list */
	list_del(&item->list);

	spin_unlock_irqrestore(&g_watchdog_ctx.lock, flags);

	/* Free memory */
	kfree(item);

	/* Check if we need to adjust work period or stop work */
	update_work_period();

	return 0;
}

/**
* watchdog_start - Start monitoring a watchdog item (Lock-free operation)
* @item: Watchdog item to start (must be valid pointer from watchdog_add)
*
* Begins timeout monitoring for the specified watchdog item. If the watchdog
* is not already active, records the current time as the start point and
* activates monitoring. If already active, this call is ignored to prevent
* timeout extension through repeated start calls.
*
* This "start-once" behavior ensures predictable timeout behavior:
* - First watchdog_start() sets the timeout baseline using current jiffies
* - Subsequent calls are ignored until watchdog_cancel() is called
* - To restart timeout counting, must call watchdog_cancel() then watchdog_start()
*
* The operation is lock-free for maximum performance on hot paths, using
* atomic operations and memory barriers to ensure thread safety. This makes
* it suitable for use in interrupt contexts and performance-critical code paths.
*
* Once started, the recovery function will be called repeatedly every work
* period after the timeout expires, until the watchdog is cancelled or removed.
*
* Context: Any context (atomic, interrupt-safe, lock-free)
* Return: 0 on success, -ENODEV if watchdog system not initialized,
*         -EINVAL if @item is NULL or invalid
*
* Example:
* @code
* // Start watchdog before critical operation
* static int my_device_critical_operation(struct my_device *dev)
* {
*     int ret;
*     
*     // Start 3-second timeout before operation
*     ret = watchdog_start(dev->watchdog);
*     if (ret) {
*         dev_err(&dev->pdev->dev, "Failed to start watchdog: %d\n", ret);
*         return ret;
*     }
*     
*     // Perform critical operation that might hang
*     ret = my_device_send_command(dev, CRITICAL_CMD);
*     
*     // Cancel watchdog on successful completion
*     if (ret == 0) {
*         watchdog_cancel(dev->watchdog);
*     }
*     // If operation failed/hung, recovery function will be called
*     
*     return ret;
* }
* 
* // Start-once behavior demonstration
* watchdog_start(item);  // Sets timeout baseline at current time
* msleep(50);
* watchdog_start(item);  // Ignored - timeout still based on first call
* msleep(50);
* watchdog_cancel(item); // Reset the watchdog
* watchdog_start(item);  // Now sets new timeout baseline
* 
* // Safe to call from interrupt context
* static irqreturn_t my_irq_handler(int irq, void *data)
* {
*     struct my_device *dev = data;
*     
*     // Start watchdog for interrupt processing timeout
*     watchdog_start(dev->irq_watchdog);
*     
*     // Process interrupt...
*     
*     return IRQ_HANDLED;
* }
* @endcode
*/
int watchdog_start(struct watchdog_item *item)
{
	if (!g_watchdog_ctx.initialized) {
		pr_err("Watchdog not initialized\n");
		return -ENODEV;
	}

	if (!item) {
		pr_err("Invalid watchdog item pointer\n");
		return -EINVAL;
	}

	/* Check if item is still valid (atomic read, no lock needed) */
	if (!atomic_read(&item->valid)) {
		pr_err("Watchdog item %p is invalid\n", item);
		return -EINVAL;
	}

	/*
	 * Lock-free start-once operation:
	 * Only set start_time and activate if not already active.
	 * This prevents timeout extension through repeated start calls.
	 */
	if (!atomic_read(&item->active)) {
		item->start_time = jiffies;
		smp_wmb(); /* Write memory barrier: start_time before active */
		atomic_set(&item->active, 1);
	}

	return 0;
}

/**
* watchdog_cancel - Stop monitoring a watchdog item (Lock-free operation)
* @item: Watchdog item to cancel (must be valid pointer from watchdog_add)
*
* Stops timeout monitoring for the specified watchdog item. The recovery
* function will no longer be called for this item, even if it was previously
* in timeout state and being called repeatedly.
*
* This operation immediately deactivates the watchdog by atomically clearing
* the active flag. The watchdog can be restarted later using watchdog_start(),
* which will establish a new timeout baseline from that point.
*
* The operation is lock-free for maximum performance on hot paths, using
* atomic operations to ensure thread safety. This makes it suitable for use
* in interrupt contexts, completion handlers, and performance-critical code paths.
*
* It is safe to call this function multiple times on the same item - subsequent
* calls on an already cancelled watchdog have no effect.
*
* Context: Any context (atomic, interrupt-safe, lock-free)
* Return: 0 on success, -ENODEV if watchdog system not initialized,
*         -EINVAL if @item is NULL or invalid
*
* Example:
* @code
* // Cancel watchdog on successful operation completion
* static int my_device_operation(struct my_device *dev)
* {
*     int ret;
*     
*     // Start 5-second timeout
*     watchdog_start(dev->watchdog);
*     
*     ret = my_device_perform_operation(dev);
*     
*     if (ret == 0) {
*         // Operation succeeded - cancel the watchdog
*         watchdog_cancel(dev->watchdog);
*         dev_dbg(&dev->pdev->dev, "Operation completed successfully\n");
*     } else {
*         // Operation failed - leave watchdog active for recovery
*         dev_err(&dev->pdev->dev, "Operation failed, watchdog will trigger recovery\n");
*     }
*     
*     return ret;
* }
* 
* // Cancel from completion callback
* static void my_async_completion(struct work_struct *work)
* {
*     struct my_device *dev = container_of(work, struct my_device, async_work);
*     
*     // Async operation completed - cancel timeout watchdog
*     watchdog_cancel(dev->async_watchdog);
*     
*     // Process completion...
* }
* 
* // Safe to call from interrupt context
* static irqreturn_t my_completion_irq(int irq, void *data)
* {
*     struct my_device *dev = data;
*     
*     // Hardware signaled completion - cancel watchdog immediately
*     watchdog_cancel(dev->hw_watchdog);
*     
*     return IRQ_HANDLED;
* }
* 
* // Multiple cancels are safe
* watchdog_start(item);
* // ... some operation ...
* watchdog_cancel(item);  // Cancels the watchdog
* watchdog_cancel(item);  // Safe, no effect
* watchdog_cancel(item);  // Safe, no effect
* @endcode
*/
int watchdog_cancel(struct watchdog_item *item)
{
	if (!g_watchdog_ctx.initialized) {
		pr_err("Watchdog not initialized\n");
		return -ENODEV;
	}

	if (!item) {
		pr_err("Invalid watchdog item pointer\n");
		return -EINVAL;
	}

	/* Check if item is still valid (atomic read, no lock needed) */
	if (!atomic_read(&item->valid)) {
		pr_err("Watchdog item %p is invalid\n", item);
		return -EINVAL;
	}

	/* Lock-free cancel operation: simply clear active flag */
	atomic_set(&item->active, 0);

	return 0;
}

/**
* update_work_period - Update work period and start/stop work as needed
*
* This internal function calculates the optimal work period by finding the
* shortest timeout among all valid watchdog items. The work period is set to
* half the shortest timeout for accurate detection, but clamped to a maximum
* frequency limit (WATCHDOG_MAX_WORK_PERIOD_MS) to prevent excessive CPU usage.
*
* Work scheduling behavior:
* - If no valid items exist: stop work completely (zero CPU overhead)
* - If items exist but work stopped: start work with calculated period
* - If period changed significantly: restart work with new period
* - If period unchanged: no action (avoid unnecessary work cancellation)
*
* The function automatically manages the work lifecycle based on the presence
* and characteristics of watchdog items. This provides:
* - Zero overhead when no watchdogs are active
* - Optimal detection accuracy for the shortest timeout
* - Protection against excessive CPU usage
* - Automatic adaptation to changing timeout requirements
*
* This function must be called whenever items are added or removed to
* maintain optimal performance and accuracy. It handles all the complexity
* of work management internally, requiring no user intervention.
*
* Context: Process context (may sleep due to work cancellation)
*
* Example behavior:
* @code
* // Initially no items - no work running
* // work_active = false, period_ms = 0
* 
* // Add first item with 2000ms timeout
* watchdog_add(2000, func1, data1);
* // update_work_period() called automatically
* // period_ms = 1000ms (2000/2), work starts
* 
* // Add item with shorter 800ms timeout  
* watchdog_add(800, func2, data2);
* // update_work_period() called automatically
* // period_ms = 400ms (800/2), work rescheduled
* 
* // Add item with very short 50ms timeout
* watchdog_add(50, func3, data3);  // This would BUG() - too short
* 
* // Add item with 120ms timeout (above minimum)
* watchdog_add(120, func3, data3);
* // update_work_period() called automatically
* // period_ms = 50ms (clamped to WATCHDOG_MAX_WORK_PERIOD_MS)
* 
* // Remove the 120ms item
* watchdog_remove(item3);
* // update_work_period() called automatically
* // period_ms = 400ms (back to 800/2), work rescheduled
* 
* // Remove all items
* watchdog_remove(item1);
* watchdog_remove(item2);
* // update_work_period() called automatically
* // work_active = false, period_ms = 0, zero overhead achieved
* @endcode
*/
static void update_work_period(void)
{
	struct watchdog_item *item;
	unsigned long flags;
	unsigned long min_timeout = ULONG_MAX;
	unsigned long new_period;
	bool has_items = false;
	bool period_changed = false;

	if (!g_watchdog_ctx.initialized) {
		return;
	}

	spin_lock_irqsave(&g_watchdog_ctx.lock, flags);

	/* Find the shortest timeout among all valid items */
	list_for_each_entry(item, &g_watchdog_ctx.item_list, list) {
		if (atomic_read(&item->valid)) {
			has_items = true;
			if (item->timeout_ms < min_timeout) {
				min_timeout = item->timeout_ms;
			}
		}
	}

	if (has_items) {
		/*
		 * Calculate new period: use min_timeout/2 for better accuracy,
		 * but clamp to WATCHDOG_MAX_WORK_PERIOD_MS to prevent overload
		 */
		new_period = max(min_timeout / 2, (unsigned long)WATCHDOG_MAX_WORK_PERIOD_MS);

		/* Check if period changed significantly to avoid thrashing */
		if (new_period != g_watchdog_ctx.period_ms) {
			period_changed = true;
			g_watchdog_ctx.period_ms = new_period;
		}

		/* Start work if not already running */
		if (!g_watchdog_ctx.work_active) {
			g_watchdog_ctx.work_active = true;
			spin_unlock_irqrestore(&g_watchdog_ctx.lock, flags);

			schedule_delayed_work(&g_watchdog_ctx.work, msecs_to_jiffies(new_period));
		} else if (period_changed) {
			spin_unlock_irqrestore(&g_watchdog_ctx.lock, flags);

			/* Cancel current work and reschedule with new period */
			cancel_delayed_work(&g_watchdog_ctx.work);
			schedule_delayed_work(&g_watchdog_ctx.work, msecs_to_jiffies(new_period));
		} else {
			spin_unlock_irqrestore(&g_watchdog_ctx.lock, flags);
		}
	} else {
		/* No valid items, stop the work completely for zero overhead */
		if (g_watchdog_ctx.work_active) {
			g_watchdog_ctx.work_active = false;
			g_watchdog_ctx.period_ms = 0;
			spin_unlock_irqrestore(&g_watchdog_ctx.lock, flags);

			cancel_delayed_work(&g_watchdog_ctx.work);
		} else {
			spin_unlock_irqrestore(&g_watchdog_ctx.lock, flags);
		}
	}
}
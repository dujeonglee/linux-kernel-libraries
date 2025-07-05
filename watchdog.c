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
 * watchdog operations. No periodic work is started until the first
 * watchdog item is added, providing zero overhead when idle.
 *
 * Return: 0 on success, -EBUSY if already initialized
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
 * the system to uninitialized state. This function is safe to call multiple
 * times and handles cleanup gracefully even if no items exist.
 *
 * All watchdog items are marked invalid before being freed to prevent
 * use-after-free issues if other code still holds references.
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
 * watchdog_add - Add a new watchdog item
 * @timeout_ms: Timeout in milliseconds (must be >= WATCHDOG_MIN_TIMEOUT_MS)
 * @recovery_func: Function to call when timeout occurs (must not be NULL)
 * @private_data: Opaque pointer passed to recovery function (can be NULL)
 *
 * Creates a new watchdog item and adds it to the monitoring system. The
 * watchdog starts in inactive state; use watchdog_start() to begin monitoring.
 *
 * If this is the first watchdog item, the periodic work is automatically
 * started. If the timeout is shorter than existing items, the work period
 * is adjusted for better accuracy.
 *
 * The timeout must be at least WATCHDOG_MIN_TIMEOUT_MS milliseconds to
 * prevent excessive CPU usage. Shorter timeouts will trigger BUG() to
 * protect system stability.
 *
 * Return: Pointer to watchdog item on success, NULL on allocation failure,
 *         or BUG() if timeout_ms < WATCHDOG_MIN_TIMEOUT_MS
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
 * watchdog_remove - Remove and free a watchdog item
 * @item: Watchdog item to remove (must be valid pointer from watchdog_add)
 *
 * Removes the specified watchdog item from monitoring and frees its memory.
 * The item is marked invalid before removal to prevent use-after-free issues.
 *
 * If this was the last watchdog item, the periodic work is automatically
 * stopped to save CPU resources. If other items remain, the work period
 * may be adjusted based on the remaining shortest timeout.
 *
 * Return: 0 on success, negative error code on failure
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
 * watchdog_start - Start monitoring a watchdog item (Lock-free)
 * @item: Watchdog item to start (must be valid pointer from watchdog_add)
 *
 * Begins timeout monitoring for the specified watchdog item. If the watchdog
 * is not already active, records the current time as the start point and
 * activates monitoring. If already active, this call is ignored to prevent
 * timeout extension through repeated start calls.
 *
 * This "start-once" behavior ensures predictable timeout behavior:
 * - First watchdog_start() sets the timeout baseline
 * - Subsequent calls are ignored until watchdog_cancel() is called
 * - To restart timeout, must call watchdog_cancel() then watchdog_start()
 *
 * Memory barriers ensure that the start time is written before the active
 * flag is set, preventing race conditions where the work function might
 * see active=1 but an uninitialized start_time.
 *
 * Return: 0 on success, negative error code on failure
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
 * watchdog_cancel - Stop monitoring a watchdog item (Lock-free)
 * @item: Watchdog item to cancel (must be valid pointer from watchdog_add)
 *
 * Stops timeout monitoring for the specified watchdog item. The recovery
 * function will no longer be called for this item. This operation is
 * lock-free for maximum performance on hot paths.
 *
 * Return: 0 on success, negative error code on failure
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
 * This function calculates the optimal work period by finding the shortest
 * timeout among all valid watchdog items. The work period is set to half
 * the shortest timeout for accurate detection, but clamped to a maximum
 * of WATCHDOG_MAX_WORK_PERIOD_MS to prevent excessive CPU usage.
 *
 * Work scheduling behavior:
 * - If no valid items exist: stop work completely (zero CPU overhead)
 * - If items exist but work stopped: start work with calculated period  
 * - If period changed significantly: restart work with new period
 * - If period unchanged: no action (avoid unnecessary work cancellation)
 *
 * This function must be called whenever items are added or removed to
 * maintain optimal performance and accuracy.
 *
 * Context: Process context, may sleep due to work cancellation
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
#include "monitor.h"

/**
 * monitor_state_changed_with_hysteresis() - Check if monitor state has changed with hysteresis
 * @item: Monitor item to check
 * @new_state: New state value returned by monitor function
 *
 * This function implements hysteresis based on consecutive count mechanism.
 * State change is recognized only when the new state value appears 
 * consecutively for the specified hysteresis count.
 *
 * Hysteresis behavior:
 * - hysteresis = 0: immediate state change recognition
 * - hysteresis = N: new state must appear N times consecutively
 *
 * The function maintains internal state:
 * - candidate_state: the potential new state being evaluated
 * - consecutive_count: number of consecutive occurrences of candidate_state
 *
 * Context: Called from monitor work function with manager lock held
 * Return: true if state change should trigger action, false otherwise
 */
static bool monitor_state_changed_with_hysteresis(struct monitor_item *item, 
                                                  unsigned long new_state)
{
    /* No hysteresis - immediate change recognition */
    if (item->hysteresis == 0) {
        return item->last_action_state != new_state;
    }
    
    /* No change from last action state */
    if (item->last_action_state == new_state) {
        item->consecutive_count = 0;
        item->candidate_state = new_state;
        return false;
    }
    
    /* Check if this matches the candidate state */
    if (item->candidate_state == new_state) {
        item->consecutive_count++;
        monitor_debug("Item %s: consecutive count %lu for state %lu (need %lu)",
                     item->name, item->consecutive_count, new_state, item->hysteresis);
        
        /* Hysteresis threshold reached - trigger action */
        if (item->consecutive_count >= item->hysteresis) {
            item->consecutive_count = 0;
            return true;
        }
    } else {
        /* New candidate state - reset counter */
        item->candidate_state = new_state;
        item->consecutive_count = 1;
        monitor_debug("Item %s: new candidate state %lu (count 1, need %lu)",
                     item->name, new_state, item->hysteresis);
    }
    
    return false;
}

/**
 * monitor_work_func() - Monitor work queue function
 * @work: Work structure embedded in delayed_work
 *
 * This is the main monitoring function that runs periodically via workqueue.
 * It iterates through all monitor items and checks their states according
 * to their individual intervals. When a state change is detected (considering
 * hysteresis), the corresponding action function is called.
 *
 * The function temporarily releases the spinlock when calling action functions
 * to allow them to sleep if needed. This means the item list could potentially
 * change during action execution, so we recheck the manager state.
 *
 * Context: Workqueue context (process context, can sleep)
 */
static void monitor_work_func(struct work_struct *work)
{
    struct monitor_manager *mgr = container_of(work, struct monitor_manager, work.work);
    struct monitor_item *item, *tmp;
    unsigned long current_time = jiffies;
    unsigned long flags;
    
    if (!mgr->running) {
        return;
    }
    
    spin_lock_irqsave(&mgr->lock, flags);
    
    /* Iterate through all monitor items */
    list_for_each_entry_safe(item, tmp, &mgr->item_list, list) {
        /* Check if it's time to monitor this item */
        if (time_after(current_time, item->last_check_time + 
                      msecs_to_jiffies(item->interval_ms))) {
            
            unsigned long new_state;
            
            /* Call monitor function */
            if (item->monitor_func) {
                new_state = item->monitor_func(item->private_data);
                item->check_count++;
                mgr->total_checks++;
                
                monitor_debug("Item %s: state %lu -> %lu", 
                             item->name, item->current_state, new_state);
                
                /* Check for state change with hysteresis */
                if (monitor_state_changed_with_hysteresis(item, new_state)) {
                    /* State changed - call action function */
                    if (item->action_func) {
                        /* Release spinlock before calling action (may sleep) */
                        spin_unlock_irqrestore(&mgr->lock, flags);
                        
                        monitor_debug("Item %s: executing action, state change %lu -> %lu",
                                     item->name, item->last_action_state, new_state);
                        
                        item->action_func(item->last_action_state, new_state, item->private_data);
                        
                        spin_lock_irqsave(&mgr->lock, flags);
                        
                        /* List might have changed - recheck manager state */
                        if (!mgr->running) {
                            spin_unlock_irqrestore(&mgr->lock, flags);
                            return;
                        }
                        
                        item->last_action_state = new_state;
                        item->action_count++;
                        mgr->total_actions++;
                    }
                }
                
                item->current_state = new_state;
                item->last_check_time = current_time;
            }
        }
    }
    
    spin_unlock_irqrestore(&mgr->lock, flags);
    
    /* Schedule next execution */
    if (mgr->running) {
        schedule_delayed_work(&mgr->work, msecs_to_jiffies(mgr->base_interval_ms));
    }
}

/**
 * monitor_manager_init() - Initialize monitor manager
 * @mgr: Pointer to monitor manager structure
 * @base_interval_ms: Base monitoring interval in milliseconds
 *
 * Initializes all components of the monitor manager including the item list,
 * delayed work structure, and spinlock. Sets the base interval which will be
 * used as the minimum monitoring interval and work scheduling interval.
 * If base_interval_ms is 0, uses DEFAULT_MONITOR_INTERVAL_MS.
 *
 * Context: Process context
 * Return: 0 on success, -EINVAL if mgr is NULL
 */
int monitor_manager_init(struct monitor_manager *mgr, unsigned long base_interval_ms)
{
    if (!mgr) {
        return -EINVAL;
    }
    
    memset(mgr, 0, sizeof(*mgr));
    
    INIT_LIST_HEAD(&mgr->item_list);
    INIT_DELAYED_WORK(&mgr->work, monitor_work_func);
    spin_lock_init(&mgr->lock);
    
    mgr->base_interval_ms = base_interval_ms ? base_interval_ms : DEFAULT_MONITOR_INTERVAL_MS;
    mgr->running = false;
    mgr->initialized = true;
    
    monitor_info("Monitor manager initialized with base interval %lu ms", 
                 mgr->base_interval_ms);
    
    return 0;
}

/**
 * monitor_manager_cleanup() - Clean up monitor manager
 * @mgr: Pointer to monitor manager structure
 *
 * Stops monitoring if active and frees all resources associated with the
 * manager. All monitor items in the list are removed and freed. After
 * cleanup, the manager must be reinitialized before use.
 *
 * Context: Process context
 */
void monitor_manager_cleanup(struct monitor_manager *mgr)
{
    struct monitor_item *item, *tmp;
    unsigned long flags;
    
    if (!mgr || !mgr->initialized) {
        return;
    }
    
    /* Stop monitoring */
    monitor_stop(mgr);
    
    /* Remove and free all items */
    spin_lock_irqsave(&mgr->lock, flags);
    list_for_each_entry_safe(item, tmp, &mgr->item_list, list) {
        list_del(&item->list);
        kfree(item);
    }
    spin_unlock_irqrestore(&mgr->lock, flags);
    
    mgr->initialized = false;
    
    monitor_info("Monitor manager cleaned up");
}

/**
 * monitor_start() - Start monitoring
 * @mgr: Pointer to monitor manager structure
 *
 * Starts the periodic monitoring by scheduling the delayed work. The work
 * will run at intervals specified by base_interval_ms and check all items
 * according to their individual intervals.
 *
 * Context: Process context
 * Return: 0 on success, -EINVAL if manager is invalid, -EALREADY if already running
 */
int monitor_start(struct monitor_manager *mgr)
{
    if (!mgr || !mgr->initialized) {
        return -EINVAL;
    }
    
    if (mgr->running) {
        return -EALREADY;
    }
    
    mgr->running = true;
    schedule_delayed_work(&mgr->work, msecs_to_jiffies(mgr->base_interval_ms));
    
    monitor_info("Monitor started");
    return 0;
}

/**
 * monitor_stop() - Stop monitoring
 * @mgr: Pointer to monitor manager structure
 *
 * Stops the periodic monitoring by setting the running flag to false and
 * canceling any pending work. Uses cancel_delayed_work_sync() to ensure
 * the work function completes before returning.
 *
 * Context: Process context (may sleep during work cancellation)
 */
void monitor_stop(struct monitor_manager *mgr)
{
    if (!mgr || !mgr->initialized) {
        return;
    }
    
    mgr->running = false;
    cancel_delayed_work_sync(&mgr->work);
    
    monitor_info("Monitor stopped");
}

/**
 * monitor_add_item() - Add a monitor item
 * @mgr: Pointer to monitor manager structure
 * @init: Pointer to initialization structure containing item parameters
 *
 * Creates a new monitor item with the specified configuration and adds it
 * to the manager's item list. The item will be included in monitoring cycles
 * once added. The interval_ms must be a multiple of the manager's base_interval_ms
 * and must be >= base_interval_ms.
 *
 * If init->interval_ms is 0, uses the manager's base_interval_ms.
 * If init->name is NULL, generates a default name based on the item pointer.
 *
 * Context: Process context
 * Return: Pointer to created monitor item on success, NULL on error
 */
struct monitor_item *monitor_add_item(struct monitor_manager *mgr, 
                                     const struct monitor_item_init *init)
{
    struct monitor_item *item;
    unsigned long flags;
    unsigned long interval_ms;
    
    if (!mgr || !mgr->initialized || !init || !init->monitor_func) {
        return NULL;
    }
    
    /* Validate interval_ms */
    interval_ms = init->interval_ms ? init->interval_ms : mgr->base_interval_ms;
    
    /* interval_ms must be multiple of base_interval_ms */
    if (interval_ms % mgr->base_interval_ms != 0) {
        monitor_err("Invalid interval %lu ms: must be multiple of base interval %lu ms",
                   interval_ms, mgr->base_interval_ms);
        return NULL;
    }
    
    /* interval_ms must be >= base_interval_ms */
    if (interval_ms < mgr->base_interval_ms) {
        monitor_err("Invalid interval %lu ms: must be >= base interval %lu ms",
                   interval_ms, mgr->base_interval_ms);
        return NULL;
    }
    
    item = kzalloc(sizeof(*item), GFP_KERNEL);
    if (!item) {
        return NULL;
    }
    
    /* Initialize item */
    INIT_LIST_HEAD(&item->list);
    item->interval_ms = interval_ms;
    item->hysteresis = init->hysteresis;
    item->monitor_func = init->monitor_func;
    item->action_func = init->action_func;
    item->private_data = init->private_data;
    
    /* Initialize state */
    item->current_state = 0;
    item->last_action_state = 0;
    item->last_check_time = jiffies;
    
    /* Initialize hysteresis state */
    item->candidate_state = 0;
    item->consecutive_count = 0;
    
    /* Initialize statistics */
    item->check_count = 0;
    item->action_count = 0;
    
    /* Set name */
    if (init->name) {
        strncpy(item->name, init->name, sizeof(item->name) - 1);
        item->name[sizeof(item->name) - 1] = '\0';
    } else {
        snprintf(item->name, sizeof(item->name), "item_%p", item);
    }
    
    spin_lock_irqsave(&mgr->lock, flags);
    
    /* Add to list */
    list_add_tail(&item->list, &mgr->item_list);
    
    spin_unlock_irqrestore(&mgr->lock, flags);
    
    monitor_info("Added monitor item '%s' (addr:%p, interval:%lu ms, hysteresis:%lu)",
                 item->name, item, item->interval_ms, item->hysteresis);
    
    return item;
}

/**
 * monitor_remove_item() - Remove a monitor item
 * @mgr: Pointer to monitor manager structure  
 * @item: Pointer to monitor item to remove
 *
 * Removes the specified monitor item from the manager's list and frees its
 * memory. The item will no longer be monitored after removal. It's safe to
 * call this function while monitoring is active.
 *
 * Context: Process context
 * Return: 0 on success, -EINVAL if parameters are invalid
 */
int monitor_remove_item(struct monitor_manager *mgr, struct monitor_item *item)
{
    unsigned long flags;
    
    if (!mgr || !mgr->initialized || !item) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&mgr->lock, flags);
    
    /* Remove from list */
    list_del(&item->list);
    
    spin_unlock_irqrestore(&mgr->lock, flags);
    
    monitor_info("Removed monitor item '%s' (addr:%p)", item->name, item);
    kfree(item);
    
    return 0;
}

/**
 * monitor_get_item_state() - Get current state of a monitor item
 * @item: Pointer to monitor item
 * @current_state: Pointer to store current state value
 *
 * Retrieves the current state value of the specified monitor item.
 * This is the most recent value returned by the item's monitor function.
 *
 * Context: Any context
 * Return: 0 on success, -EINVAL if parameters are invalid
 */
int monitor_get_item_state(struct monitor_item *item, unsigned long *current_state)
{
    if (!item || !current_state) {
        return -EINVAL;
    }
    
    *current_state = item->current_state;
    return 0;
}

/**
 * monitor_get_item_stats() - Get statistics for a monitor item
 * @item: Pointer to monitor item
 * @check_count: Pointer to store check count (can be NULL)
 * @action_count: Pointer to store action count (can be NULL)
 *
 * Retrieves the statistics for the specified monitor item including the
 * total number of monitor function calls and action function calls.
 *
 * Context: Any context
 * Return: 0 on success, -EINVAL if item is NULL
 */
int monitor_get_item_stats(struct monitor_item *item,
                          unsigned long *check_count, unsigned long *action_count)
{
    if (!item) {
        return -EINVAL;
    }
    
    if (check_count) {
        *check_count = item->check_count;
    }
    if (action_count) {
        *action_count = item->action_count;
    }
    
    return 0;
}

/**
 * monitor_get_manager_stats() - Get statistics for the monitor manager
 * @mgr: Pointer to monitor manager structure
 * @total_checks: Pointer to store total check count (can be NULL)
 * @total_actions: Pointer to store total action count (can be NULL)  
 * @active_items: Pointer to store active item count (can be NULL)
 *
 * Retrieves the overall statistics for the monitor manager including total
 * checks across all items, total actions executed, and the number of
 * currently active monitor items.
 *
 * Context: Any context (briefly holds spinlock)
 * Return: 0 on success, -EINVAL if manager is invalid
 */
int monitor_get_manager_stats(struct monitor_manager *mgr,
                             unsigned long *total_checks, unsigned long *total_actions,
                             unsigned int *active_items)
{
    struct monitor_item *item;
    unsigned long flags;
    unsigned int count = 0;
    
    if (!mgr || !mgr->initialized) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&mgr->lock, flags);
    
    if (total_checks) {
        *total_checks = mgr->total_checks;
    }
    if (total_actions) {
        *total_actions = mgr->total_actions;
    }
    
    if (active_items) {
        list_for_each_entry(item, &mgr->item_list, list) {
            count++;
        }
        *active_items = count;
    }
    
    spin_unlock_irqrestore(&mgr->lock, flags);
    return 0;
}

/* Module metadata */
MODULE_AUTHOR("Dujeong Lee<dujeong.lee82@gmail.com>");
MODULE_DESCRIPTION("Linux Kernel Monitor Library");
MODULE_VERSION(MONITOR_VERSION);
MODULE_LICENSE("GPL");
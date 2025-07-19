#include "state_watcher.h"

/**
 * state_watcher_state_changed_with_hysteresis() - Check if state has changed with hysteresis
 * @item: Watch item to check
 * @new_state: New state value returned by state function
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
 * Context: Called from state watcher work function with watcher lock held
 * Return: true if state change should trigger action, false otherwise
 */
static bool state_watcher_state_changed_with_hysteresis(struct watch_item *item, 
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
        state_watcher_debug("Item %s: consecutive count %lu for state %lu (need %lu)",
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
        state_watcher_debug("Item %s: new candidate state %lu (count 1, need %lu)",
                           item->name, new_state, item->hysteresis);
    }

    return false;
}

/**
 * state_watcher_work_func() - State watcher work queue function
 * @work: Work structure embedded in delayed_work
 *
 * This is the main watching function that runs periodically via workqueue.
 * It iterates through all watch items and checks their states according
 * to their individual intervals. When a state change is detected (considering
 * hysteresis), the corresponding action function is called.
 *
 * The function temporarily releases the spinlock when calling action functions
 * to allow them to sleep if needed. This means the item list could potentially
 * change during action execution, so we recheck the watcher state.
 *
 * Context: Workqueue context (process context, can sleep)
 */
static void state_watcher_work_func(struct work_struct *work)
{
    struct state_watcher *watcher = container_of(work, struct state_watcher, work.work);
    struct watch_item *item, *tmp;
    unsigned long current_time = jiffies;
    unsigned long flags;

    if (!watcher->running) {
        return;
    }

    spin_lock_irqsave(&watcher->lock, flags);

    /* Iterate through all watch items */
    list_for_each_entry_safe(item, tmp, &watcher->item_list, list) {
        /* Check if it's time to watch this item */
        if (time_after(current_time, item->last_check_time + 
                      msecs_to_jiffies(item->interval_ms))) {
            unsigned long new_state;

            /* Check if forced state has expired */
            if (item->is_forced && time_after(current_time, item->forced_state_expire_time)) {
                item->is_forced = false;
                state_watcher_debug("Item %s: forced state expired, resuming normal watching",
                                   item->name);
            }

            /* Call state function */
            if (item->state_func) {
                unsigned long state_result = item->state_func(item->private_data);
                bool state_changed;

                item->check_count++;
                watcher->total_checks++;

                /* Use forced state if active, otherwise use state result */
                if (item->is_forced) {
                    new_state = item->forced_state;
                    state_watcher_debug("Item %s: using forced state %lu (state func returned %lu)", 
                                       item->name, new_state, state_result);
                } else {
                    new_state = state_result;
                    state_watcher_debug("Item %s: state %lu -> %lu", 
                                       item->name, item->current_state, new_state);
                }

                /* Check for state change with hysteresis (ignore hysteresis for forced state) */
                if (item->is_forced) {
                    /* For forced state, ignore hysteresis and trigger action immediately */
                    state_changed = (item->last_action_state != new_state);
                    state_watcher_debug("Item %s: forced state bypass hysteresis, state change %lu -> %lu",
                                       item->name, item->last_action_state, new_state);
                } else {
                    /* Normal hysteresis checking */
                    state_changed = state_watcher_state_changed_with_hysteresis(item, new_state);
                }

                if (state_changed) {
                    /* State changed - call action function */
                    if (item->action_func) {
                        /* Release spinlock before calling action (may sleep) */
                        spin_unlock_irqrestore(&watcher->lock, flags);

                        state_watcher_debug("Item %s: executing action, state change %lu -> %lu",
                                           item->name, item->last_action_state, new_state);

                        item->action_func(item->last_action_state, new_state, item->private_data);

                        spin_lock_irqsave(&watcher->lock, flags);

                        /* List might have changed - recheck watcher state */
                        if (!watcher->running) {
                            spin_unlock_irqrestore(&watcher->lock, flags);
                            return;
                        }

                        item->last_action_state = new_state;
                        item->action_count++;
                        watcher->total_actions++;
                    }
                }

                item->current_state = new_state;
                item->last_check_time = current_time;
            }
        }
    }

    spin_unlock_irqrestore(&watcher->lock, flags);

    /* Schedule next execution */
    if (watcher->running) {
        schedule_delayed_work(&watcher->work, msecs_to_jiffies(watcher->base_interval_ms));
    }
}

/**
 * state_watcher_init() - Initialize state watcher
 * @watcher: Pointer to state watcher structure
 * @base_interval_ms: Base watching interval in milliseconds
 *
 * Initializes all components of the state watcher including the item list,
 * delayed work structure, and spinlock. Sets the base interval which will be
 * used as the minimum watching interval and work scheduling interval.
 * If base_interval_ms is 0, uses DEFAULT_STATE_WATCHER_INTERVAL_MS.
 *
 * Context: Process context
 * Return: 0 on success, -EINVAL if watcher is NULL
 */
int state_watcher_init(struct state_watcher *watcher, unsigned long base_interval_ms)
{
    if (!watcher) {
        return -EINVAL;
    }

    memset(watcher, 0, sizeof(*watcher));

    INIT_LIST_HEAD(&watcher->item_list);
    INIT_DELAYED_WORK(&watcher->work, state_watcher_work_func);
    spin_lock_init(&watcher->lock);

    watcher->base_interval_ms = base_interval_ms ? base_interval_ms : DEFAULT_STATE_WATCHER_INTERVAL_MS;
    watcher->running = false;
    watcher->initialized = true;

    state_watcher_info("State watcher initialized with base interval %lu ms", 
                       watcher->base_interval_ms);

    return 0;
}

/**
 * state_watcher_force_state() - Force a watch item to a specific state
 * @item: Pointer to watch item
 * @forced_state: State value to force
 * @duration_ms: Duration in milliseconds to maintain the forced state
 *
 * Forces the watch item to report the specified state for the given duration.
 * During this time, the actual state function is still called but its return
 * value is ignored. Hysteresis is bypassed for forced states, causing immediate
 * action function calls when the state changes. After the duration expires, 
 * normal watching resumes with hysteresis behavior restored.
 * If the item already has a forced state, it will be overwritten.
 *
 * Context: Any context
 * Return: 0 on success, -EINVAL if parameters are invalid
 */
int state_watcher_force_state(struct watch_item *item, unsigned long forced_state, 
                              unsigned long duration_ms)
{
    unsigned long current_time = jiffies;

    if (!item || duration_ms == 0) {
        return -EINVAL;
    }

    item->forced_state = forced_state;
    item->forced_state_expire_time = current_time + msecs_to_jiffies(duration_ms);
    item->is_forced = true;

    state_watcher_info("Item %s: forced state %lu for %lu ms", 
                       item->name, forced_state, duration_ms);

    return 0;
}

/**
 * state_watcher_clear_forced_state() - Clear forced state and resume normal watching
 * @item: Pointer to watch item
 *
 * Immediately clears any forced state and resumes normal watching behavior.
 * If no forced state is active, this function has no effect.
 *
 * Context: Any context
 * Return: 0 on success, -EINVAL if item is NULL
 */
int state_watcher_clear_forced_state(struct watch_item *item)
{
    if (!item) {
        return -EINVAL;
    }

    if (item->is_forced) {
        item->is_forced = false;
        state_watcher_info("Item %s: forced state cleared, resuming normal watching", 
                           item->name);
    }

    return 0;
}

/**
 * state_watcher_is_state_forced() - Check if a watch item has forced state
 * @item: Pointer to watch item
 * @remaining_ms: Pointer to store remaining time in milliseconds (can be NULL)
 *
 * Checks if the watch item currently has a forced state active.
 * Optionally returns the remaining time before forced state expires.
 * If the forced state has already expired, it will be automatically cleared.
 *
 * Context: Any context
 * Return: true if state is forced, false otherwise
 */
bool state_watcher_is_state_forced(struct watch_item *item, unsigned long *remaining_ms)
{
    unsigned long current_time = jiffies;

    if (!item) {
        return false;
    }

    /* Check if forced state has expired */
    if (item->is_forced && time_after(current_time, item->forced_state_expire_time)) {
        item->is_forced = false;
        state_watcher_debug("Item %s: forced state expired during check", item->name);
    }

    if (item->is_forced && remaining_ms) {
        unsigned long remaining_jiffies = item->forced_state_expire_time - current_time;
        *remaining_ms = jiffies_to_msecs(remaining_jiffies);
    }

    return item->is_forced;
}

/**
 * state_watcher_cleanup() - Clean up state watcher
 * @watcher: Pointer to state watcher structure
 *
 * Stops watching if active and frees all resources associated with the
 * watcher. All watch items in the list are removed and freed. After
 * cleanup, the watcher must be reinitialized before use.
 *
 * Context: Process context
 */
void state_watcher_cleanup(struct state_watcher *watcher)
{
    struct watch_item *item, *tmp;
    unsigned long flags;

    if (!watcher || !watcher->initialized) {
        return;
    }

    /* Stop watching */
    state_watcher_stop(watcher);

    /* Remove and free all items */
    spin_lock_irqsave(&watcher->lock, flags);
    list_for_each_entry_safe(item, tmp, &watcher->item_list, list) {
        list_del(&item->list);
        kfree(item);
    }
    spin_unlock_irqrestore(&watcher->lock, flags);

    watcher->initialized = false;

    state_watcher_info("State watcher cleaned up");
}

/**
 * state_watcher_start() - Start watching
 * @watcher: Pointer to state watcher structure
 *
 * Starts the periodic watching by scheduling the delayed work. The work
 * will run at intervals specified by base_interval_ms and check all items
 * according to their individual intervals.
 *
 * Context: Process context
 * Return: 0 on success, -EINVAL if watcher is invalid, -EALREADY if already running
 */
int state_watcher_start(struct state_watcher *watcher)
{
    if (!watcher || !watcher->initialized) {
        return -EINVAL;
    }

    if (watcher->running) {
        return -EALREADY;
    }

    watcher->running = true;
    schedule_delayed_work(&watcher->work, msecs_to_jiffies(watcher->base_interval_ms));

    state_watcher_info("State watcher started");
    return 0;
}

/**
 * state_watcher_stop() - Stop watching
 * @watcher: Pointer to state watcher structure
 *
 * Stops the periodic watching by setting the running flag to false and
 * canceling any pending work. Uses cancel_delayed_work_sync() to ensure
 * the work function completes before returning.
 *
 * Context: Process context (may sleep during work cancellation)
 */
void state_watcher_stop(struct state_watcher *watcher)
{
    if (!watcher || !watcher->initialized) {
        return;
    }

    watcher->running = false;
    cancel_delayed_work_sync(&watcher->work);

    state_watcher_info("State watcher stopped");
}

/**
 * state_watcher_add_item() - Add a watch item
 * @watcher: Pointer to state watcher structure
 * @init: Pointer to initialization structure containing item parameters
 *
 * Creates a new watch item with the specified configuration and adds it
 * to the watcher's item list. The item will be included in watching cycles
 * once added. The interval_ms must be a multiple of the watcher's base_interval_ms
 * and must be >= base_interval_ms.
 *
 * If init->interval_ms is 0, uses the watcher's base_interval_ms.
 * If init->name is NULL, generates a default name based on the item pointer.
 *
 * Context: Process context
 * Return: Pointer to created watch item on success, NULL on error
 */
struct watch_item *state_watcher_add_item(struct state_watcher *watcher, 
                                          const struct watch_item_init *init)
{
    struct watch_item *item;
    unsigned long flags;
    unsigned long interval_ms;

    if (!watcher || !watcher->initialized || !init || !init->state_func) {
        return NULL;
    }

    /* Validate interval_ms */
    interval_ms = init->interval_ms ? init->interval_ms : watcher->base_interval_ms;

    /* interval_ms must be multiple of base_interval_ms */
    if (interval_ms % watcher->base_interval_ms != 0) {
        state_watcher_err("Invalid interval %lu ms: must be multiple of base interval %lu ms",
                         interval_ms, watcher->base_interval_ms);
        return NULL;
    }

    /* interval_ms must be >= base_interval_ms */
    if (interval_ms < watcher->base_interval_ms) {
        state_watcher_err("Invalid interval %lu ms: must be >= base interval %lu ms",
                         interval_ms, watcher->base_interval_ms);
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
    item->state_func = init->state_func;
    item->action_func = init->action_func;
    item->private_data = init->private_data;

    /* Initialize state */
    item->current_state = 0;
    item->last_action_state = 0;
    item->last_check_time = jiffies;

    /* Initialize hysteresis state */
    item->candidate_state = 0;
    item->consecutive_count = 0;

    /* Initialize forced state management */
    item->forced_state = 0;
    item->forced_state_expire_time = 0;
    item->is_forced = false;

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

    spin_lock_irqsave(&watcher->lock, flags);

    /* Add to list */
    list_add_tail(&item->list, &watcher->item_list);

    spin_unlock_irqrestore(&watcher->lock, flags);

    state_watcher_info("Added watch item '%s' (addr:%p, interval:%lu ms, hysteresis:%lu)",
                       item->name, item, item->interval_ms, item->hysteresis);

    return item;
}

/**
 * state_watcher_remove_item() - Remove a watch item
 * @watcher: Pointer to state watcher structure  
 * @item: Pointer to watch item to remove
 *
 * Removes the specified watch item from the watcher's list and frees its
 * memory. The item will no longer be watched after removal. It's safe to
 * call this function while watching is active.
 *
 * Context: Process context
 * Return: 0 on success, -EINVAL if parameters are invalid
 */
int state_watcher_remove_item(struct state_watcher *watcher, struct watch_item *item)
{
    unsigned long flags;

    if (!watcher || !watcher->initialized || !item) {
        return -EINVAL;
    }

    spin_lock_irqsave(&watcher->lock, flags);

    /* Remove from list */
    list_del(&item->list);

    spin_unlock_irqrestore(&watcher->lock, flags);

    state_watcher_info("Removed watch item '%s' (addr:%p)", item->name, item);
    kfree(item);

    return 0;
}

/**
 * state_watcher_get_item_state() - Get current state of a watch item
 * @item: Pointer to watch item
 * @current_state: Pointer to store current state value
 *
 * Retrieves the current state value of the specified watch item.
 * This is the most recent value returned by the item's state function.
 *
 * Context: Any context
 * Return: 0 on success, -EINVAL if parameters are invalid
 */
int state_watcher_get_item_state(struct watch_item *item, unsigned long *current_state)
{
    if (!item || !current_state) {
        return -EINVAL;
    }

    *current_state = item->current_state;
    return 0;
}

/**
 * state_watcher_get_item_stats() - Get statistics for a watch item
 * @item: Pointer to watch item
 * @check_count: Pointer to store check count (can be NULL)
 * @action_count: Pointer to store action count (can be NULL)
 *
 * Retrieves the statistics for the specified watch item including the
 * total number of state function calls and action function calls.
 *
 * Context: Any context
 * Return: 0 on success, -EINVAL if item is NULL
 */
int state_watcher_get_item_stats(struct watch_item *item,
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
 * state_watcher_get_stats() - Get statistics for the state watcher
 * @watcher: Pointer to state watcher structure
 * @total_checks: Pointer to store total check count (can be NULL)
 * @total_actions: Pointer to store total action count (can be NULL)  
 * @active_items: Pointer to store active item count (can be NULL)
 *
 * Retrieves the overall statistics for the state watcher including total
 * checks across all items, total actions executed, and the number of
 * currently active watch items.
 *
 * Context: Any context (briefly holds spinlock)
 * Return: 0 on success, -EINVAL if watcher is invalid
 */
int state_watcher_get_stats(struct state_watcher *watcher,
                            unsigned long *total_checks, unsigned long *total_actions,
                            unsigned int *active_items)
{
    struct watch_item *item;
    unsigned long flags;
    unsigned int count = 0;

    if (!watcher || !watcher->initialized) {
        return -EINVAL;
    }

    spin_lock_irqsave(&watcher->lock, flags);

    if (total_checks) {
        *total_checks = watcher->total_checks;
    }
    if (total_actions) {
        *total_actions = watcher->total_actions;
    }

    if (active_items) {
        list_for_each_entry(item, &watcher->item_list, list) {
            count++;
        }
        *active_items = count;
    }

    spin_unlock_irqrestore(&watcher->lock, flags);
    return 0;
}
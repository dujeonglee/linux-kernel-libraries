#ifndef _STATE_WATCHER_H
#define _STATE_WATCHER_H

#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/spinlock.h>

/**
 * DOC: State Watcher Library Overview
 *
 * The State Watcher Library provides a framework for periodic watching of system
 * states with configurable intervals and hysteresis support. It allows
 * registration of state functions that check system conditions and trigger
 * actions when state changes occur.
 *
 * Key features:
 * - Configurable watching intervals per item
 * - Hysteresis support to avoid state flapping
 * - Workqueue-based periodic execution
 * - Thread-safe operation with spinlocks
 * - Statistics collection for watching performance
 */

/** Default watching interval in milliseconds */
#define DEFAULT_STATE_WATCHER_INTERVAL_MS 1000
/** Default hysteresis value (no hysteresis) */
#define DEFAULT_HYSTERESIS 0

/**
 * typedef state_func_t - State function type
 * @private_data: User-provided private data
 *
 * This function type is used for state functions that check system state.
 * The function should return the current state value.
 *
 * Return: Current state value as unsigned long
 */
typedef unsigned long (*state_func_t)(void *private_data);

/**
 * typedef action_func_t - Action function type
 * @old_state: Previous state value
 * @new_state: New state value
 * @private_data: User-provided private data
 *
 * This function type is used for action functions that are called when
 * a state change is detected (considering hysteresis).
 */
typedef void (*action_func_t)(unsigned long old_state, unsigned long new_state, void *private_data);

/**
 * struct watch_item - Watch item structure
 * @list: List node for linking items in the watcher
 * @interval_ms: Watching interval in milliseconds
 * @hysteresis: Hysteresis value (consecutive count threshold)
 * @state_func: Pointer to state function
 * @action_func: Pointer to action function
 * @private_data: User-provided private data
 * @current_state: Current state value
 * @last_action_state: State value when last action was executed
 * @last_check_time: Timestamp of last check (in jiffies)
 * @candidate_state: Candidate state for hysteresis checking
 * @consecutive_count: Count of consecutive identical states
 * @name: Item name (user-defined, null-terminated)
 * @check_count: Total number of checks performed
 * @action_count: Total number of actions executed
 * @forced_state: Forced state value
 * @forced_state_expire_time: Expiration time for forced state
 * @is_forced: Flag indicating if state is currently forced
 *
 * This structure represents a single watch item with all its configuration,
 * state, and statistics information.
 */
struct watch_item {
    struct list_head list;

    /* User configuration parameters */
    unsigned long interval_ms;
    unsigned long hysteresis;
    state_func_t state_func;
    action_func_t action_func;
    void *private_data;

    /* Internal state management */
    unsigned long current_state;
    unsigned long last_action_state;
    unsigned long last_check_time;

    /* Hysteresis management */
    unsigned long candidate_state;
    unsigned long consecutive_count;

    /* Forced state management */
    unsigned long forced_state;
    unsigned long forced_state_expire_time;
    bool is_forced;

    /* Identifier */
    char name[32];

    /* Statistics */
    unsigned long check_count;
    unsigned long action_count;
};

/**
 * struct state_watcher - State watcher structure
 * @item_list: Head of the watch items list
 * @work: Delayed work structure for periodic execution
 * @lock: Spinlock for thread-safe operations
 * @base_interval_ms: Base check interval in milliseconds
 * @running: Flag indicating if watching is active
 * @initialized: Flag indicating if watcher is initialized
 * @total_checks: Total number of checks across all items
 * @total_actions: Total number of actions across all items
 *
 * This structure manages a collection of watch items and provides
 * the infrastructure for periodic watching execution.
 */
struct state_watcher {
    struct list_head item_list;
    struct delayed_work work;
    spinlock_t lock;
    
    /* Watcher configuration */
    unsigned long base_interval_ms;
    
    /* State flags */
    bool running;
    bool initialized;
    
    /* Statistics */
    unsigned long total_checks;
    unsigned long total_actions;
};

/**
 * struct watch_item_init - Watch item initialization structure
 * @name: Item name (must be null-terminated)
 * @interval_ms: Watching interval in milliseconds
 * @hysteresis: Hysteresis value (consecutive count threshold)
 * @state_func: Pointer to state function
 * @action_func: Pointer to action function
 * @private_data: User-provided private data
 *
 * This structure is used to pass initialization parameters when adding
 * a new watch item to the watcher.
 */
struct watch_item_init {
    const char *name;
    unsigned long interval_ms;
    unsigned long hysteresis;
    state_func_t state_func;
    action_func_t action_func;
    void *private_data;
};

/**
 * state_watcher_init() - Initialize state watcher
 * @watcher: Pointer to state watcher structure
 * @base_interval_ms: Base watching interval in milliseconds
 *
 * Initializes the state watcher structure, including the item list,
 * work queue, and spinlock. Must be called before using the watcher.
 *
 * Return: 0 on success, negative error code on failure
 */
int state_watcher_init(struct state_watcher *watcher, unsigned long base_interval_ms);

/**
 * state_watcher_cleanup() - Clean up state watcher
 * @watcher: Pointer to state watcher structure
 *
 * Stops watching if active and cleans up all resources associated
 * with the watcher, including removing all watch items.
 */
void state_watcher_cleanup(struct state_watcher *watcher);

/**
 * state_watcher_start() - Start watching
 * @watcher: Pointer to state watcher structure
 *
 * Starts the periodic watching by scheduling the work queue.
 * The watcher must be initialized before calling this function.
 *
 * Return: 0 on success, negative error code on failure
 */
int state_watcher_start(struct state_watcher *watcher);

/**
 * state_watcher_stop() - Stop watching
 * @watcher: Pointer to state watcher structure
 *
 * Stops the periodic watching by canceling the work queue.
 * This function is safe to call multiple times.
 */
void state_watcher_stop(struct state_watcher *watcher);

/**
 * state_watcher_add_item() - Add a watch item
 * @watcher: Pointer to state watcher structure
 * @init: Pointer to initialization structure
 *
 * Creates and adds a new watch item to the watcher with the specified
 * configuration. The item will be included in the periodic watching
 * cycle once added.
 *
 * Return: Pointer to the created watch item on success, NULL on failure
 */
struct watch_item *state_watcher_add_item(struct state_watcher *watcher, 
                                          const struct watch_item_init *init);

/**
 * state_watcher_remove_item() - Remove a watch item
 * @watcher: Pointer to state watcher structure
 * @item: Pointer to watch item to remove
 *
 * Removes the specified watch item from the watcher and frees
 * its resources. The item will no longer be watched.
 *
 * Return: 0 on success, negative error code on failure
 */
int state_watcher_remove_item(struct state_watcher *watcher, struct watch_item *item);

/**
 * state_watcher_get_item_state() - Get current state of a watch item
 * @item: Pointer to watch item
 * @current_state: Pointer to store current state value
 *
 * Retrieves the current state value of the specified watch item.
 *
 * Return: 0 on success, negative error code on failure
 */
int state_watcher_get_item_state(struct watch_item *item, unsigned long *current_state);

/**
 * state_watcher_get_item_stats() - Get statistics for a watch item
 * @item: Pointer to watch item
 * @check_count: Pointer to store check count
 * @action_count: Pointer to store action count
 *
 * Retrieves the statistics (check count and action count) for the
 * specified watch item.
 *
 * Return: 0 on success, negative error code on failure
 */
int state_watcher_get_item_stats(struct watch_item *item,
                                 unsigned long *check_count, unsigned long *action_count);

/**
 * state_watcher_get_stats() - Get statistics for the state watcher
 * @watcher: Pointer to state watcher structure
 * @total_checks: Pointer to store total check count
 * @total_actions: Pointer to store total action count
 * @active_items: Pointer to store active item count
 *
 * Retrieves the overall statistics for the state watcher, including
 * total checks, total actions, and number of active items.
 *
 * Return: 0 on success, negative error code on failure
 */
int state_watcher_get_stats(struct state_watcher *watcher,
                            unsigned long *total_checks, unsigned long *total_actions,
                            unsigned int *active_items);

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
 * Return: 0 on success, -EINVAL if parameters are invalid
 */
int state_watcher_force_state(struct watch_item *item, unsigned long forced_state, 
                              unsigned long duration_ms);

/**
 * state_watcher_clear_forced_state() - Clear forced state and resume normal watching
 * @item: Pointer to watch item
 *
 * Immediately clears any forced state and resumes normal watching behavior.
 * If no forced state is active, this function has no effect.
 *
 * Return: 0 on success, -EINVAL if item is NULL
 */
int state_watcher_clear_forced_state(struct watch_item *item);

/**
 * state_watcher_is_state_forced() - Check if a watch item has forced state
 * @item: Pointer to watch item
 * @remaining_ms: Pointer to store remaining time in milliseconds (can be NULL)
 *
 * Checks if the watch item currently has a forced state active.
 * Optionally returns the remaining time before forced state expires.
 * If the forced state has already expired, it will be automatically cleared.
 *
 * Return: true if state is forced, false otherwise
 */
bool state_watcher_is_state_forced(struct watch_item *item, unsigned long *remaining_ms);

#endif /* _STATE_WATCHER_H */
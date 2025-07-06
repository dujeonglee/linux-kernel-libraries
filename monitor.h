#ifndef _MONITOR_H
#define _MONITOR_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/jiffies.h>

/**
 * DOC: Monitor Library Overview
 *
 * The Monitor Library provides a framework for periodic monitoring of system
 * states with configurable intervals and hysteresis support. It allows
 * registration of monitor functions that check system conditions and trigger
 * actions when state changes occur.
 *
 * Key features:
 * - Configurable monitoring intervals per item
 * - Hysteresis support to avoid state flapping
 * - Workqueue-based periodic execution
 * - Thread-safe operation with spinlocks
 * - Statistics collection for monitoring performance
 */

/** Monitor library version */
#define MONITOR_VERSION "1.0.0"

/** Default monitoring interval in milliseconds */
#define DEFAULT_MONITOR_INTERVAL_MS 1000
/** Default hysteresis value (no hysteresis) */
#define DEFAULT_HYSTERESIS 0

/**
 * typedef monitor_func_t - Monitor function type
 * @private_data: User-provided private data
 *
 * This function type is used for monitor functions that check system state.
 * The function should return the current state value.
 *
 * Return: Current state value as unsigned long
 */
typedef unsigned long (*monitor_func_t)(void *private_data);

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
 * struct monitor_item - Monitor item structure
 * @list: List node for linking items in the manager
 * @interval_ms: Monitoring interval in milliseconds
 * @hysteresis: Hysteresis value (consecutive count threshold)
 * @monitor_func: Pointer to monitor function
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
 *
 * This structure represents a single monitor item with all its configuration,
 * state, and statistics information.
 */
struct monitor_item {
    struct list_head list;
    
    /* User configuration parameters */
    unsigned long interval_ms;
    unsigned long hysteresis;
    monitor_func_t monitor_func;
    action_func_t action_func;
    void *private_data;
    
    /* Internal state management */
    unsigned long current_state;
    unsigned long last_action_state;
    unsigned long last_check_time;
    
    /* Hysteresis management */
    unsigned long candidate_state;
    unsigned long consecutive_count;
    
    /* Identifier */
    char name[32];
    
    /* Statistics */
    unsigned long check_count;
    unsigned long action_count;
};

/**
 * struct monitor_manager - Monitor manager structure
 * @item_list: Head of the monitor items list
 * @work: Delayed work structure for periodic execution
 * @lock: Spinlock for thread-safe operations
 * @base_interval_ms: Base check interval in milliseconds
 * @running: Flag indicating if monitoring is active
 * @initialized: Flag indicating if manager is initialized
 * @total_checks: Total number of checks across all items
 * @total_actions: Total number of actions across all items
 *
 * This structure manages a collection of monitor items and provides
 * the infrastructure for periodic monitoring execution.
 */
struct monitor_manager {
    struct list_head item_list;
    struct delayed_work work;
    spinlock_t lock;
    
    /* Manager configuration */
    unsigned long base_interval_ms;
    
    /* State flags */
    bool running;
    bool initialized;
    
    /* Statistics */
    unsigned long total_checks;
    unsigned long total_actions;
};

/**
 * struct monitor_item_init - Monitor item initialization structure
 * @name: Item name (must be null-terminated)
 * @interval_ms: Monitoring interval in milliseconds
 * @hysteresis: Hysteresis value (consecutive count threshold)
 * @monitor_func: Pointer to monitor function
 * @action_func: Pointer to action function
 * @private_data: User-provided private data
 *
 * This structure is used to pass initialization parameters when adding
 * a new monitor item to the manager.
 */
struct monitor_item_init {
    const char *name;
    unsigned long interval_ms;
    unsigned long hysteresis;
    monitor_func_t monitor_func;
    action_func_t action_func;
    void *private_data;
};

/**
 * monitor_manager_init() - Initialize monitor manager
 * @mgr: Pointer to monitor manager structure
 * @base_interval_ms: Base monitoring interval in milliseconds
 *
 * Initializes the monitor manager structure, including the item list,
 * work queue, and spinlock. Must be called before using the manager.
 *
 * Return: 0 on success, negative error code on failure
 */
int monitor_manager_init(struct monitor_manager *mgr, unsigned long base_interval_ms);

/**
 * monitor_manager_cleanup() - Clean up monitor manager
 * @mgr: Pointer to monitor manager structure
 *
 * Stops monitoring if active and cleans up all resources associated
 * with the manager, including removing all monitor items.
 */
void monitor_manager_cleanup(struct monitor_manager *mgr);

/**
 * monitor_start() - Start monitoring
 * @mgr: Pointer to monitor manager structure
 *
 * Starts the periodic monitoring by scheduling the work queue.
 * The manager must be initialized before calling this function.
 *
 * Return: 0 on success, negative error code on failure
 */
int monitor_start(struct monitor_manager *mgr);

/**
 * monitor_stop() - Stop monitoring
 * @mgr: Pointer to monitor manager structure
 *
 * Stops the periodic monitoring by canceling the work queue.
 * This function is safe to call multiple times.
 */
void monitor_stop(struct monitor_manager *mgr);

/**
 * monitor_add_item() - Add a monitor item
 * @mgr: Pointer to monitor manager structure
 * @init: Pointer to initialization structure
 *
 * Creates and adds a new monitor item to the manager with the specified
 * configuration. The item will be included in the periodic monitoring
 * cycle once added.
 *
 * Return: Pointer to the created monitor item on success, NULL on failure
 */
struct monitor_item *monitor_add_item(struct monitor_manager *mgr, 
                                     const struct monitor_item_init *init);

/**
 * monitor_remove_item() - Remove a monitor item
 * @mgr: Pointer to monitor manager structure
 * @item: Pointer to monitor item to remove
 *
 * Removes the specified monitor item from the manager and frees
 * its resources. The item will no longer be monitored.
 *
 * Return: 0 on success, negative error code on failure
 */
int monitor_remove_item(struct monitor_manager *mgr, struct monitor_item *item);

/**
 * monitor_get_item_state() - Get current state of a monitor item
 * @item: Pointer to monitor item
 * @current_state: Pointer to store current state value
 *
 * Retrieves the current state value of the specified monitor item.
 *
 * Return: 0 on success, negative error code on failure
 */
int monitor_get_item_state(struct monitor_item *item, unsigned long *current_state);

/**
 * monitor_get_item_stats() - Get statistics for a monitor item
 * @item: Pointer to monitor item
 * @check_count: Pointer to store check count
 * @action_count: Pointer to store action count
 *
 * Retrieves the statistics (check count and action count) for the
 * specified monitor item.
 *
 * Return: 0 on success, negative error code on failure
 */
int monitor_get_item_stats(struct monitor_item *item,
                          unsigned long *check_count, unsigned long *action_count);

/**
 * monitor_get_manager_stats() - Get statistics for the monitor manager
 * @mgr: Pointer to monitor manager structure
 * @total_checks: Pointer to store total check count
 * @total_actions: Pointer to store total action count
 * @active_items: Pointer to store active item count
 *
 * Retrieves the overall statistics for the monitor manager, including
 * total checks, total actions, and number of active items.
 *
 * Return: 0 on success, negative error code on failure
 */
int monitor_get_manager_stats(struct monitor_manager *mgr,
                             unsigned long *total_checks, unsigned long *total_actions,
                             unsigned int *active_items);

/**
 * monitor_state_changed_with_hysteresis() - Check if state changed with hysteresis
 * @item: Pointer to monitor item
 * @new_state: New state value to check
 *
 * Determines if a state change should be considered valid based on the
 * configured hysteresis value. This helps prevent state flapping by
 * requiring consecutive identical states before triggering an action.
 *
 * If hysteresis is 0, any state change is immediately considered valid.
 * If hysteresis is > 0, the new state must be detected consecutively
 * for the specified number of times before being considered valid.
 *
 * Return: true if state change should trigger action, false otherwise
 */
static inline bool monitor_state_changed_with_hysteresis(struct monitor_item *item, 
                                                        unsigned long new_state)
{
    /* No hysteresis - immediate change recognition */
    if (item->hysteresis == 0) {
        return item->last_action_state != new_state;
    }
    
    /* No change from last action state */
    if (item->last_action_state == new_state) {
        item->consecutive_count = 0;
        return false;
    }
    
    /* Check if this matches the candidate state */
    if (item->candidate_state == new_state) {
        item->consecutive_count++;
        monitor_debug("Item %s: consecutive count %lu for state %lu (need %lu)",
                     item->name, item->consecutive_count, new_state, item->hysteresis);
        
        /* Hysteresis threshold reached */
        if (item->consecutive_count >= item->hysteresis) {
            item->consecutive_count = 0;
            return true;
        }
    } else {
        /* New candidate state */
        item->candidate_state = new_state;
        item->consecutive_count = 1;
        monitor_debug("Item %s: new candidate state %lu (count 1, need %lu)",
                     item->name, new_state, item->hysteresis);
    }
    
    return false;
}

/* Debug and logging macros */
#ifdef DEBUG
#define monitor_debug(fmt, ...) \
    printk(KERN_DEBUG "monitor: " fmt "\n", ##__VA_ARGS__)
#else
#define monitor_debug(fmt, ...) do { } while (0)
#endif

#define monitor_info(fmt, ...) \
    printk(KERN_INFO "monitor: " fmt "\n", ##__VA_ARGS__)

#define monitor_warn(fmt, ...) \
    printk(KERN_WARNING "monitor: " fmt "\n", ##__VA_ARGS__)

#define monitor_err(fmt, ...) \
    printk(KERN_ERR "monitor: " fmt "\n", ##__VA_ARGS__)

#endif /* _MONITOR_H */
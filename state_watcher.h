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

/**
 * DEFAULT_STATE_WATCHER_INTERVAL_MS - Default base watching interval
 *
 * This macro defines the default base interval in milliseconds for the state
 * watcher framework. This value is used when:
 * 1. state_watcher_init() is called with base_interval_ms = 0
 * 2. watch_item_init.interval_ms is set to 0 when adding items
 *
 * The base interval serves as:
 * - The minimum granularity for all watching operations
 * - The work queue scheduling interval
 * - The default interval for watch items that don't specify their own
 *
 * All individual watch item intervals must be multiples of this base interval.
 * For example, if base interval is 1000ms, valid item intervals are:
 * 1000ms, 2000ms, 3000ms, etc.
 *
 * Performance considerations:
 * - Lower values provide more responsive watching but increase CPU overhead
 * - Higher values reduce CPU load but decrease responsiveness
 * - 200ms (0.2 second) provides a good balance for most use cases
 *
 * Range: Typically 100ms to 10000ms depending on system requirements
 * Units: milliseconds
 * Default: 200 (0.2 second)
 *
 * Example:
 * @code
 * struct state_watcher watcher;
 * // Initialize with default 200ms interval
 * state_watcher_init(&watcher, 0);
 * 
 * // Add item using default interval
 * struct watch_item_init init = {
 *     .name = "battery_monitor",
 *     .interval_ms = 0,  // Uses DEFAULT_STATE_WATCHER_INTERVAL_MS
 *     .state_func = battery_check_func,
 *     .action_func = battery_action_func
 * };
 * state_watcher_add_item(&watcher, &init);
 * @endcode
 */
#define DEFAULT_STATE_WATCHER_INTERVAL_MS 200

/**
 * DEFAULT_HYSTERESIS - Default hysteresis value for state change detection
 *
 * This macro defines the default hysteresis value used when creating watch items
 * without explicitly specifying hysteresis behavior. A hysteresis value of 0
 * means immediate state change recognition without any delay or filtering.
 *
 * Hysteresis mechanism:
 * - hysteresis = 0: State change triggers action immediately (no hysteresis)
 * - hysteresis = N: New state must appear N consecutive times before triggering action
 *
 * Purpose:
 * - Prevents excessive action calls due to state flapping/oscillation
 * - Provides stability in noisy or unstable state readings
 * - Reduces CPU overhead from frequent state transitions
 *
 * When to use different values:
 * - 0: For stable states that don't oscillate (temperature, battery level)
 * - 1-3: For slightly noisy signals (network connectivity, sensor readings)  
 * - 4+: For very noisy or rapidly changing states
 *
 * The hysteresis counter resets whenever:
 * - The state returns to the previous action state
 * - A completely different candidate state is detected
 *
 * Note: Forced states bypass hysteresis and trigger actions immediately
 *
 * Default: 0 (no hysteresis - immediate state change recognition)
 *
 * Example:
 * @code
 * // Item with default hysteresis (immediate action)
 * struct watch_item_init immediate_init = {
 *     .name = "battery_level",
 *     .hysteresis = DEFAULT_HYSTERESIS,  // 0 - immediate
 *     .state_func = battery_state_func,
 *     .action_func = battery_action_func
 * };
 * 
 * // Item with custom hysteresis (3 consecutive readings)
 * struct watch_item_init stable_init = {
 *     .name = "network_status", 
 *     .hysteresis = 3,  // Requires 3 consecutive state changes
 *     .state_func = network_state_func,
 *     .action_func = network_action_func
 * };
 * 
 * state_watcher_add_item(&watcher, &immediate_init);
 * state_watcher_add_item(&watcher, &stable_init);
 * @endcode
 */
#define DEFAULT_HYSTERESIS 0

/**
 * state_func_t - State function callback type
 * @private_data: User-provided private data passed during watch item initialization
 *
 * This function type defines the callback interface for state checking functions
 * used by the state watcher framework. The state function is called periodically
 * according to the watch item's interval to determine the current system state.
 *
 * Function responsibilities:
 * - Read and evaluate the current state of the monitored resource
 * - Return a numeric representation of the state as unsigned long
 * - Handle any necessary locking or synchronization internally
 * - Be lightweight and avoid blocking operations when possible
 *
 * State value interpretation:
 * - The returned value is opaque to the state watcher framework
 * - Meaning is defined by the specific implementation and corresponding action function
 * - Common patterns: boolean states (0/1), enumerated states, percentage values, counts
 * - State changes are detected by comparing consecutive return values
 *
 * Context considerations:
 * - Called from workqueue context (process context, can sleep if necessary)
 * - May be called frequently depending on watch item interval
 * - Should minimize execution time to avoid affecting other watch items
 * - Temporary spinlock release during execution allows sleeping if needed
 *
 * Error handling:
 * - Return a consistent "error state" value if reading fails
 * - The framework does not distinguish between valid and error states
 * - Error handling should be implemented in the action function
 *
 * Return: Current state value as unsigned long
 *
 * Example:
 * @code
 * // Battery level monitoring (0-100%)
 * static unsigned long battery_state_func(void *private_data)
 * {
 *     struct battery_info *info = (struct battery_info *)private_data;
 *     int level = read_battery_level(info->device_id);
 *     return (level < 0) ? 0 : (unsigned long)level;
 * }
 * 
 * // Network interface status (0=down, 1=up)
 * static unsigned long network_state_func(void *private_data)
 * {
 *     char *interface = (char *)private_data;
 *     struct net_device *dev = dev_get_by_name(&init_net, interface);
 *     if (!dev) return 0;
 *     unsigned long state = (dev->flags & IFF_UP) ? 1 : 0;
 *     dev_put(dev);
 *     return state;
 * }
 * 
 * // Temperature monitoring (degrees Celsius)
 * static unsigned long temperature_state_func(void *private_data)
 * {
 *     int sensor_id = *(int *)private_data;
 *     long temp = thermal_zone_get_temp(sensor_id);
 *     return (temp < 0) ? 0 : (unsigned long)(temp / 1000); // milli-celsius to celsius
 * }
 * @endcode
 */
typedef unsigned long (*state_func_t)(void *private_data);

/**
 * action_func_t - Action function callback type
 * @old_state: Previous state value that triggered the last action
 * @new_state: New state value that triggered this action call
 * @private_data: User-provided private data passed during watch item initialization
 *
 * This function type defines the callback interface for action functions that are
 * executed when a state change is detected by the state watcher framework. The
 * action function is called after hysteresis conditions are met (if configured)
 * and represents the response to meaningful state transitions.
 *
 * Function responsibilities:
 * - React to state changes with appropriate system actions
 * - Handle state transition logic based on old_state and new_state values
 * - Perform necessary logging, notifications, or system adjustments
 * - Manage any cleanup or initialization required for state transitions
 *
 * State transition handling:
 * - old_state: The state value when the action function was last called
 * - new_state: The current state value that triggered this call
 * - Both values are determined by the corresponding state_func_t return values
 * - For first-time execution, old_state equals the initial state (typically 0)
 *
 * Context considerations:
 * - Called from workqueue context with spinlocks temporarily released
 * - Can perform blocking operations (sleep, wait for resources, etc.)
 * - Should avoid excessive execution time to prevent delaying other watch items
 * - May be called frequently if states change rapidly
 *
 * Synchronization:
 * - The state watcher framework handles basic synchronization
 * - Action functions should implement additional locking if accessing shared resources
 * - Multiple action functions may execute concurrently for different watch items
 *
 * Error handling:
 * - Action functions should handle errors gracefully without crashing
 * - Failed actions do not affect the state watcher's operation
 * - Consider logging errors for debugging purposes
 *
 * Return: void (no return value)
 *
 * Example:
 * @code
 * // Battery level action (critical/warning/normal states)
 * static void battery_action_func(unsigned long old_state, unsigned long new_state, void *private_data)
 * {
 *     if (new_state <= 10 && old_state > 10) {
 *         pr_crit("Battery critical: %lu%% - immediate action required!\n", new_state);
 *         trigger_emergency_shutdown();
 *     } else if (new_state <= 20 && old_state > 20) {
 *         pr_warn("Battery low: %lu%% - consider charging\n", new_state);
 *         send_low_battery_notification();
 *     } else if (new_state > 20 && old_state <= 20) {
 *         pr_info("Battery recovered: %lu%%\n", new_state);
 *         cancel_low_battery_warnings();
 *     }
 * }
 * 
 * // Network interface action (link up/down handling)
 * static void network_action_func(unsigned long old_state, unsigned long new_state, void *private_data)
 * {
 *     char *interface = (char *)private_data;
 *     
 *     if (new_state == 1 && old_state == 0) {
 *         pr_info("Network interface %s: link up\n", interface);
 *         restart_network_services(interface);
 *         update_routing_table(interface, true);
 *     } else if (new_state == 0 && old_state == 1) {
 *         pr_warn("Network interface %s: link down\n", interface);
 *         stop_network_services(interface);
 *         update_routing_table(interface, false);
 *     }
 * }
 * 
 * // Temperature action (thermal management)
 * static void thermal_action_func(unsigned long old_state, unsigned long new_state, void *private_data)
 * {
 *     int *sensor_id = (int *)private_data;
 *     
 *     if (new_state > 85 && old_state <= 85) {
 *         pr_warn("Temperature high on sensor %d: %lu°C - enabling cooling\n", 
 *                 *sensor_id, new_state);
 *         enable_thermal_cooling(*sensor_id);
 *     } else if (new_state <= 75 && old_state > 75) {
 *         pr_info("Temperature normal on sensor %d: %lu°C - disabling cooling\n",
 *                 *sensor_id, new_state);
 *         disable_thermal_cooling(*sensor_id);
 *     }
 * }
 * @endcode
 */
typedef void (*action_func_t)(unsigned long old_state, unsigned long new_state, void *private_data);

/**
 * struct watch_item - Individual watch item for state monitoring
 * @list: List node for linking items in the state watcher's item list
 * @interval_ms: Watching interval in milliseconds (must be multiple of base_interval_ms)
 * @hysteresis: Hysteresis value - consecutive count threshold for state change recognition
 * @state_func: Pointer to state function that reads current state
 * @action_func: Pointer to action function called on state changes (can be NULL)
 * @private_data: User-provided private data passed to state and action functions
 * @current_state: Most recent state value returned by state_func
 * @last_action_state: State value when action_func was last called
 * @last_check_time: Timestamp of last state check (in jiffies)
 * @candidate_state: Potential new state being evaluated for hysteresis
 * @consecutive_count: Number of consecutive occurrences of candidate_state
 * @forced_state: Forced state value when is_forced is true
 * @forced_state_expire_time: Expiration time for forced state (in jiffies)
 * @is_forced: Flag indicating if state is currently forced
 * @name: Human-readable identifier for this watch item (null-terminated)
 * @check_count: Total number of state function calls performed
 * @action_count: Total number of action function calls executed
 *
 * This structure represents a single monitoring item within the state watcher
 * framework. Each item has its own configuration, state tracking, and statistics.
 * Items are managed in a linked list and processed periodically according to
 * their individual intervals.
 *
 * Lifecycle:
 * 1. Created and initialized via state_watcher_add_item()
 * 2. Added to watcher's item list and monitored periodically
 * 3. State and action functions called according to interval and hysteresis
 * 4. Removed and freed via state_watcher_remove_item()
 *
 * State management:
 * - current_state: Always reflects the latest state_func return value
 * - last_action_state: Updated only when action_func is called
 * - State changes trigger actions based on last_action_state vs new state
 *
 * Hysteresis behavior:
 * - candidate_state: Tracks potential new state
 * - consecutive_count: Counts consecutive occurrences of candidate_state
 * - Action triggers when consecutive_count reaches hysteresis threshold
 * - Resets when state returns to last_action_state or changes to different value
 *
 * Forced state feature:
 * - Temporarily overrides state_func return value
 * - Bypasses hysteresis for immediate action triggering
 * - Automatically expires after specified duration
 * - Normal operation resumes after expiration or manual clearing
 *
 * Thread safety:
 * - Access protected by state watcher's spinlock
 * - Safe concurrent access from multiple contexts
 * - State and action functions called with spinlock released
 *
 * Example:
 * @code
 * // Create and configure a battery monitoring item
 * struct watch_item_init battery_init = {
 *     .name = "battery_monitor",
 *     .interval_ms = 5000,           // Check every 5 seconds
 *     .hysteresis = 2,               // Require 2 consecutive readings
 *     .state_func = battery_level_func,
 *     .action_func = battery_action_func,
 *     .private_data = &battery_device
 * };
 * 
 * struct watch_item *battery_item = state_watcher_add_item(&watcher, &battery_init);
 * 
 * // Force low battery state for testing (10% for 30 seconds)
 * state_watcher_force_state(battery_item, 10, 30000);
 * 
 * // Check current state and statistics
 * unsigned long current_state, checks, actions;
 * state_watcher_get_item_state(battery_item, &current_state);
 * state_watcher_get_item_stats(battery_item, &checks, &actions);
 * printk("Battery: %lu%%, %lu checks, %lu actions\n", current_state, checks, actions);
 * 
 * // Remove when no longer needed
 * state_watcher_remove_item(&watcher, battery_item);
 * @endcode
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
 * struct state_watcher - Main state watcher framework structure
 * @item_list: Head of linked list containing all watch_item structures
 * @work: Delayed work structure for periodic execution via workqueue
 * @lock: Spinlock protecting concurrent access to watcher state and item list
 * @base_interval_ms: Base check interval in milliseconds for work scheduling
 * @running: Flag indicating if periodic watching is currently active
 * @initialized: Flag indicating if watcher has been properly initialized
 * @total_checks: Cumulative count of all state function calls across all items
 * @total_actions: Cumulative count of all action function calls across all items
 *
 * This structure serves as the central coordinator for the state monitoring
 * framework. It manages a collection of watch items and orchestrates their
 * periodic execution through the Linux kernel workqueue mechanism.
 *
 * Architecture:
 * - Single watcher can manage multiple watch items with different intervals
 * - All item intervals must be multiples of base_interval_ms
 * - Work function runs every base_interval_ms and checks items individually
 * - Thread-safe operation through spinlock protection
 *
 * Work scheduling:
 * - Uses delayed_work for periodic execution in process context
 * - base_interval_ms determines work queue scheduling frequency
 * - Individual items checked according to their own interval requirements
 * - Automatic rescheduling continues while running flag is true
 *
 * Synchronization model:
 * - Spinlock protects item list modifications and watcher state
 * - Lock temporarily released during state/action function calls
 * - Allows blocking operations in user callbacks
 * - Prevents race conditions during start/stop operations
 *
 * State management:
 * - initialized: Set once during state_watcher_init(), cleared in cleanup
 * - running: Atomic flag controlling work execution lifecycle
 * - Uses cmpxchg for atomic start/stop operations
 *
 * Statistics tracking:
 * - total_checks: Incremented for every state function call
 * - total_actions: Incremented for every action function call
 * - Provides overall framework usage metrics
 * - Individual item statistics maintained separately
 *
 * Memory management:
 * - Watcher structure typically statically allocated or embedded
 * - Watch items dynamically allocated/freed during add/remove
 * - Cleanup function handles proper resource deallocation
 *
 * Example:
 * @code
 * // Declare and initialize a state watcher
 * static struct state_watcher system_watcher;
 * 
 * static int __init system_monitor_init(void)
 * {
 *     int ret;
 *     
 *     // Initialize with 1 second base interval
 *     ret = state_watcher_init(&system_watcher, 1000);
 *     if (ret) {
 *         pr_err("Failed to initialize watcher: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     // Add monitoring items
 *     struct watch_item_init battery_init = {
 *         .name = "battery",
 *         .interval_ms = 5000,  // 5 seconds (5x base)
 *         .hysteresis = 2,
 *         .state_func = battery_check,
 *         .action_func = battery_action
 *     };
 *     state_watcher_add_item(&system_watcher, &battery_init);
 *     
 *     // Start monitoring
 *     ret = state_watcher_start(&system_watcher);
 *     if (ret) {
 *         pr_err("Failed to start watcher: %d\n", ret);
 *         state_watcher_cleanup(&system_watcher);
 *         return ret;
 *     }
 *     
 *     pr_info("System monitor started successfully\n");
 *     return 0;
 * }
 * 
 * static void __exit system_monitor_exit(void)
 * {
 *     unsigned long total_checks, total_actions;
 *     unsigned int active_items;
 *     
 *     // Get final statistics
 *     state_watcher_get_stats(&system_watcher, &total_checks, 
 *                            &total_actions, &active_items);
 *     pr_info("Stopping: %lu checks, %lu actions, %u items\n",
 *             total_checks, total_actions, active_items);
 *     
 *     // Clean shutdown
 *     state_watcher_stop(&system_watcher);
 *     state_watcher_cleanup(&system_watcher);
 * }
 * @endcode
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
 * struct watch_item_init - Watch item initialization parameters
 * @name: Human-readable identifier for the watch item (null-terminated string)
 * @interval_ms: Watching interval in milliseconds (must be multiple of base_interval_ms)
 * @hysteresis: Consecutive count threshold for state change recognition
 * @state_func: Pointer to state function that reads current state (required)
 * @action_func: Pointer to action function called on state changes (optional, can be NULL)
 * @private_data: User-provided private data passed to state and action functions
 *
 * This structure is used to pass initialization parameters when creating a new
 * watch item via state_watcher_add_item(). It provides a clean interface for
 * configuring all aspects of a watch item before adding it to the watcher.
 *
 * Parameter validation:
 * - name: If NULL, a default name based on item pointer is generated
 * - interval_ms: If 0, uses watcher's base_interval_ms as default
 * - interval_ms: Must be >= base_interval_ms and multiple of base_interval_ms
 * - hysteresis: Can be 0 (immediate) or positive value for filtering
 * - state_func: Must not be NULL (required for monitoring)
 * - action_func: Can be NULL if only state tracking is needed
 * - private_data: Can be NULL if functions don't need additional context
 *
 * Initialization patterns:
 * - Structure initialization: Use designated initializers for clarity
 * - Zero initialization: Unspecified fields automatically set to 0/NULL
 * - Default values: interval_ms=0 and name=NULL trigger default behavior
 * - Function pointers: Must point to valid functions with correct signatures
 *
 * Memory considerations:
 * - Structure is typically stack-allocated and copied during item creation
 * - name string is copied into watch_item.name[32] buffer
 * - private_data pointer is stored as-is (user manages lifetime)
 * - No dynamic allocation required for initialization structure
 *
 * Common usage patterns:
 * - Designated initializers for clear parameter specification
 * - Compound literals for inline initialization
 * - Array of initializers for batch item creation
 * - Conditional initialization based on runtime configuration
 *
 * Example:
 * @code
 * // Basic item initialization with all parameters
 * struct watch_item_init battery_init = {
 *     .name = "battery_monitor",
 *     .interval_ms = 5000,           // Check every 5 seconds
 *     .hysteresis = 3,               // Require 3 consecutive readings
 *     .state_func = battery_level_func,
 *     .action_func = battery_action_func,
 *     .private_data = &battery_device_info
 * };
 * 
 * // Using defaults (interval_ms=0 uses base interval, name=NULL gets auto-generated)
 * struct watch_item_init temp_init = {
 *     .state_func = temperature_func,
 *     .action_func = thermal_action_func,
 *     .hysteresis = 1
 * };
 * 
 * // Monitoring-only item (no action function)
 * struct watch_item_init stats_init = {
 *     .name = "cpu_stats",
 *     .interval_ms = 1000,
 *     .state_func = cpu_usage_func,
 *     .action_func = NULL             // Only track, don't react
 * };
 * 
 * // Batch initialization
 * static struct watch_item_init sensor_inits[] = {
 *     { .name = "temp_cpu", .state_func = cpu_temp_func, .hysteresis = 2 },
 *     { .name = "temp_gpu", .state_func = gpu_temp_func, .hysteresis = 2 },
 *     { .name = "fan_speed", .state_func = fan_speed_func, .hysteresis = 0 }
 * };
 * 
 * for (int i = 0; i < ARRAY_SIZE(sensor_inits); i++) {
 *     state_watcher_add_item(&watcher, &sensor_inits[i]);
 * }
 * @endcode
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
 * state_watcher_init() - Initialize state watcher framework
 */
int state_watcher_init(struct state_watcher *watcher, unsigned long base_interval_ms);

/**
 * state_watcher_cleanup() - Clean up state watcher and free all resources
 */
void state_watcher_cleanup(struct state_watcher *watcher);

/**
 * state_watcher_start() - Start periodic monitoring
 */
int state_watcher_start(struct state_watcher *watcher);

/**
 * state_watcher_stop() - Stop periodic monitoring
 */
void state_watcher_stop(struct state_watcher *watcher);

/**
 * state_watcher_add_item() - Add a new watch item to the watcher
 */
struct watch_item *state_watcher_add_item(struct state_watcher *watcher, 
                                          const struct watch_item_init *init);

/**
 * state_watcher_remove_item() - Remove a watch item from the watcher
 */
int state_watcher_remove_item(struct state_watcher *watcher, struct watch_item *item);

/**
 * state_watcher_get_item_state() - Get current state of a watch item
 */
int state_watcher_get_item_state(struct watch_item *item, unsigned long *current_state);

/**
 * state_watcher_get_item_stats() - Get statistics for a watch item
 */
int state_watcher_get_item_stats(struct watch_item *item,
                                 unsigned long *check_count, unsigned long *action_count);

/**
 * state_watcher_get_stats() - Get overall statistics for the state watcher
 */
int state_watcher_get_stats(struct state_watcher *watcher,
                            unsigned long *total_checks, unsigned long *total_actions,
                            unsigned int *active_items);

/**
 * state_watcher_force_state() - Force a watch item to report a specific state
 */
int state_watcher_force_state(struct watch_item *item, unsigned long forced_state, 
                              unsigned long duration_ms);

/**
 * state_watcher_clear_forced_state() - Clear forced state and resume normal monitoring
 */
int state_watcher_clear_forced_state(struct watch_item *item);

/**
 * state_watcher_is_state_forced() - Check if a watch item has an active forced state
 */
bool state_watcher_is_state_forced(struct watch_item *item, unsigned long *remaining_ms);

#endif /* _STATE_WATCHER_H */
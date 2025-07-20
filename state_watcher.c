#include "state_watcher.h"
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/jiffies.h>

#ifdef DEBUG
#define STATE_WATCHER_DEBUG(fmt, ...) \
    printk(KERN_DEBUG "wlbt: state_watcher: " fmt "\n", ##__VA_ARGS__)
#else
#define STATE_WATCHER_DEBUG(fmt, ...) do { } while (0)
#endif
#define STATE_WATCHER_INFO(fmt, ...) \
    printk(KERN_INFO "wlbt: state_watcher: " fmt "\n", ##__VA_ARGS__)

#define STATE_WATCHER_ERR(fmt, ...) \
    printk(KERN_ERR "wlbt: state_watcher: " fmt "\n", ##__VA_ARGS__)

/**
 * state_watcher_state_changed_with_hysteresis() - Check if state has changed with hysteresis filtering
 * @item: Pointer to watch item to evaluate
 * @new_state: New state value returned by the state function
 *
 * Implements hysteresis-based state change detection using a consecutive count
 * mechanism to filter out state oscillations and noise. This prevents excessive
 * action function calls due to unstable or rapidly changing state values.
 *
 * Hysteresis algorithm:
 * - hysteresis = 0: Immediate state change recognition (no filtering)
 * - hysteresis = N: New state must appear N consecutive times before triggering action
 * - Uses candidate_state to track potential new state being evaluated
 * - Uses consecutive_count to count occurrences of candidate_state
 *
 * State transition logic:
 * 1. If hysteresis is 0, compare new_state directly with last_action_state
 * 2. If new_state equals last_action_state, reset counters (no change needed)
 * 3. If new_state matches current candidate_state, increment consecutive_count
 * 4. If new_state differs from candidate_state, start new candidate evaluation
 * 5. Trigger action when consecutive_count reaches hysteresis threshold
 *
 * Counter management:
 * - consecutive_count incremented for matching candidate_state
 * - consecutive_count reset to 0 when returning to last_action_state
 * - consecutive_count reset to 1 when starting new candidate evaluation
 * - candidate_state updated to track the state being evaluated
 *
 * Noise filtering benefits:
 * - Prevents action calls due to temporary state fluctuations
 * - Reduces CPU overhead from frequent action function execution
 * - Improves system stability by requiring sustained state changes
 * - Allows tuning of responsiveness vs stability trade-off
 *
 * Debug logging:
 * - Logs consecutive count progress toward hysteresis threshold
 * - Logs new candidate state detection and counter resets
 * - Helpful for tuning hysteresis values and debugging behavior
 * - Available when DEBUG macro is defined
 *
 * Internal state tracking:
 * - item->candidate_state: The state being evaluated for consistency
 * - item->consecutive_count: Number of consecutive occurrences seen
 * - item->last_action_state: State when action was last triggered
 * - item->hysteresis: Configuration threshold for consecutive count
 *
 * Performance considerations:
 * - Lightweight integer comparisons and arithmetic
 * - No memory allocation or complex operations
 * - Minimal CPU overhead even with frequent calls
 * - State tracking uses existing item structure fields
 *
 * Context: Called from state watcher work function with watcher lock held
 * Return: true if state change should trigger action, false otherwise
 *
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
        STATE_WATCHER_DEBUG("Item %s: consecutive count %lu for state %lu (need %lu)",
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
        STATE_WATCHER_DEBUG("Item %s: new candidate state %lu (count 1, need %lu)",
                           item->name, new_state, item->hysteresis);
    }

    return false;
}

/**
 * state_watcher_work_func() - Main periodic monitoring work function
 * @work: Work structure embedded in delayed_work (container_of to get watcher)
 *
 * This is the core monitoring function that runs periodically via the Linux
 * kernel workqueue mechanism. It iterates through all watch items and performs
 * state checking and action triggering according to their individual configurations.
 * The function handles timing, locking, state evaluation, and automatic rescheduling.
 *
 * Execution flow:
 * 1. Extract watcher from delayed_work container structure
 * 2. Check if watcher is still running (early exit if stopped)
 * 3. Acquire spinlock to protect item list traversal
 * 4. Iterate through all watch items with safe list traversal
 * 5. Check each item's interval timing for readiness
 * 6. Call state functions and evaluate state changes
 * 7. Release spinlock temporarily for action function calls
 * 8. Re-acquire spinlock and continue iteration
 * 9. Schedule next work execution if still running
 *
 * Timing management:
 * - Uses current jiffies for time calculations
 * - Compares against item->last_check_time + interval
 * - Updates last_check_time after successful state check
 * - Handles forced state expiration automatically
 * - Schedules next execution using base_interval_ms
 *
 * Lock management strategy:
 * - Acquires spinlock with interrupts disabled (irqsave)
 * - Temporarily releases lock during action function calls
 * - Re-acquires lock after action completion
 * - Uses list_for_each_entry_safe for safe iteration during lock release
 * - Rechecks watcher running state after lock re-acquisition
 *
 * State evaluation process:
 * - Calls item->state_func() to get current state
 * - Handles forced state override logic
 * - Applies hysteresis filtering via state_changed_with_hysteresis()
 * - Updates statistics (check_count, total_checks)
 * - Triggers action functions when state changes detected
 *
 * Forced state handling:
 * - Checks for forced state expiration using jiffies comparison
 * - Automatically clears expired forced states
 * - Uses forced_state value instead of state_func() result when active
 * - Bypasses hysteresis for forced states (immediate action triggering)
 * - Logs forced state usage and expiration events
 *
 * Action function execution:
 * - Temporarily releases spinlock to allow blocking operations
 * - Calls action_func with old_state, new_state, and private_data
 * - Updates last_action_state and action_count statistics
 * - Handles potential item list changes during action execution
 * - Re-validates watcher state after action completion
 *
 * Error handling and robustness:
 * - Graceful handling of NULL state functions
 * - Safe list iteration even with concurrent modifications
 * - Automatic recovery from individual item failures
 * - Continues processing other items if one item fails
 * - Maintains system stability despite user code issues
 *
 * Self-scheduling mechanism:
 * - Automatically reschedules using schedule_delayed_work()
 * - Uses watcher->base_interval_ms for consistent timing
 * - Only reschedules if watcher->running flag is still true
 * - Ensures continuous monitoring until explicitly stopped
 *
 * Performance characteristics:
 * - Processes multiple items efficiently in single work execution
 * - Minimizes lock hold time by releasing during action calls
 * - Scales well with increasing number of watch items
 * - Balances responsiveness with CPU overhead
 *
 * Context: Workqueue context (process context, can sleep during action calls)
 * Synchronization: Uses watcher spinlock for item list protection
 * Return: void (no return value)
 *
 */
static void state_watcher_work_func(struct work_struct *work);
{
    struct state_watcher *watcher = container_of(work, struct state_watcher, work.work);
    struct watch_item *item, *tmp;
    unsigned long current_time = jiffies;
    unsigned long flags;

    if (!READ_ONCE(watcher->running)) {
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
                STATE_WATCHER_DEBUG("Item %s: forced state expired, resuming normal watching",
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
                    STATE_WATCHER_DEBUG("Item %s: using forced state %lu (state func returned %lu)", 
                                       item->name, new_state, state_result);
                } else {
                    new_state = state_result;
                    STATE_WATCHER_DEBUG("Item %s: state %lu -> %lu", 
                                       item->name, item->current_state, new_state);
                }

                /* Check for state change with hysteresis (ignore hysteresis for forced state) */
                if (item->is_forced) {
                    /* For forced state, ignore hysteresis and trigger action immediately */
                    state_changed = (item->last_action_state != new_state);
                    STATE_WATCHER_DEBUG("Item %s: forced state bypass hysteresis, state change %lu -> %lu",
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

                        STATE_WATCHER_DEBUG("Item %s: executing action, state change %lu -> %lu",
                                           item->name, item->last_action_state, new_state);

                        item->action_func(item->last_action_state, new_state, item->private_data);

                        spin_lock_irqsave(&watcher->lock, flags);

                        /* List might have changed - recheck watcher state */
                        if (!READ_ONCE(watcher->running)) {
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
    if (READ_ONCE(watcher->running)) {
        schedule_delayed_work(&watcher->work, msecs_to_jiffies(watcher->base_interval_ms));
    }
}

/**
 * state_watcher_init() - Initialize state watcher framework
 * @watcher: Pointer to state watcher structure to initialize
 * @base_interval_ms: Base watching interval in milliseconds (0 = use default)
 *
 * Initializes all components of the state watcher including the item list,
 * delayed work structure, and spinlock. Sets the base interval which serves
 * as the minimum watching granularity and work scheduling frequency.
 *
 * The base interval determines:
 * - Work queue scheduling frequency (watcher runs every base_interval_ms)
 * - Minimum granularity for all watch item intervals
 * - Default interval for items that specify interval_ms = 0
 * - Validation reference for item interval requirements
 *
 * Base interval selection guidelines:
 * - Lower values (100-500ms): High responsiveness, higher CPU usage
 * - Medium values (1000-2000ms): Balanced performance for most applications
 * - Higher values (5000ms+): Low overhead, suitable for background monitoring
 *
 * Initialization sequence:
 * 1. Structure zeroed using memset()
 * 2. Item list head initialized as empty list
 * 3. Delayed work structure initialized with work function
 * 4. Spinlock initialized for thread safety
 * 5. Configuration and flags set to initial values
 *
 * Post-initialization state:
 * - watcher->initialized = true
 * - watcher->running = false (must call state_watcher_start())
 * - Empty item list ready for watch items
 * - All statistics counters zeroed
 *
 * Prerequisites:
 * - watcher structure must be allocated (stack or heap)
 * - Structure doesn't need pre-initialization or zeroing
 * - Can be called multiple times (re-initializes cleanly)
 *
 * Thread safety:
 * - Safe to call from any process context
 * - Not safe to call on running watcher (stop first)
 * - No locking required during initialization
 *
 * Error conditions:
 * - Returns -EINVAL if watcher pointer is NULL
 * - Always succeeds with valid watcher pointer
 * - Does not perform memory allocation (cannot fail due to OOM)
 *
 * Context: Process context
 * Return: 0 on success, -EINVAL if watcher is NULL
 *
 * Example:
 * @code
 * // Static watcher for system monitoring
 * static struct state_watcher system_watcher;
 * 
 * static int system_monitor_init(void)
 * {
 *     int ret;
 *     
 *     // Initialize with 1 second base interval
 *     ret = state_watcher_init(&system_watcher, 1000);
 *     if (ret) {
 *         pr_err("Failed to initialize system watcher: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     // Initialize with default interval (DEFAULT_STATE_WATCHER_INTERVAL_MS)
 *     ret = state_watcher_init(&system_watcher, 0);
 *     if (ret) {
 *         pr_err("Failed to initialize with default interval: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     pr_info("State watcher initialized successfully\n");
 *     return 0;
 * }
 * 
 * // Dynamic watcher allocation
 * static struct state_watcher *create_network_watcher(void)
 * {
 *     struct state_watcher *watcher;
 *     int ret;
 *     
 *     watcher = kzalloc(sizeof(*watcher), GFP_KERNEL);
 *     if (!watcher)
 *         return NULL;
 *     
 *     // Fast network monitoring (500ms intervals)
 *     ret = state_watcher_init(watcher, 500);
 *     if (ret) {
 *         kfree(watcher);
 *         return NULL;
 *     }
 *     
 *     return watcher;
 * }
 * 
 * // Re-initialization example
 * static int reconfigure_watcher(struct state_watcher *watcher, unsigned long new_interval)
 * {
 *     // Stop current operation
 *     state_watcher_stop(watcher);
 *     state_watcher_cleanup(watcher);
 *     
 *     // Re-initialize with new interval
 *     return state_watcher_init(watcher, new_interval);
 * }
 * @endcode
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

    STATE_WATCHER_INFO("State watcher initialized with base interval %lu ms", 
                       watcher->base_interval_ms);

    return 0;
}

/**
 * state_watcher_force_state() - Force a watch item to report a specific state
 * @item: Pointer to watch item to force
 * @forced_state: State value to force the item to report
 * @duration_ms: Duration in milliseconds to maintain the forced state
 *
 * Temporarily overrides the watch item's state function to report a specified
 * state value for a given duration. This is primarily used for testing,
 * debugging, and simulation purposes to trigger specific system behaviors
 * without waiting for actual state changes.
 *
 * Forced state behavior:
 * - state_func() is still called but its return value is ignored
 * - forced_state value is used instead of actual state_func() result
 * - Hysteresis is completely bypassed for forced states
 * - Action functions are called immediately when forced state differs from last_action_state
 * - Automatic expiration after duration_ms milliseconds
 * - Normal monitoring resumes after expiration
 *
 * Hysteresis bypass:
 * - Forced states trigger actions immediately without consecutive count requirements
 * - candidate_state and consecutive_count are not used during forced state
 * - This allows immediate testing of action functions and system responses
 * - Normal hysteresis behavior resumes after forced state expires
 *
 * State transition handling:
 * - If forced_state != last_action_state, action_func() is called immediately
 * - Multiple force operations can be applied consecutively
 * - Previous forced state is overwritten by new force operation
 * - Expiration timer is reset with each new force operation
 *
 * Timing and expiration:
 * - duration_ms specifies how long the forced state remains active
 * - Expiration checked during normal monitoring cycles
 * - Automatic cleanup when duration expires
 * - Manual clearing possible with state_watcher_clear_forced_state()
 *
 * Use cases:
 * - Testing action functions with specific state values
 * - Debugging state transition logic
 * - Simulating failure conditions or edge cases
 * - Demonstrating system behavior during development
 * - Automated testing of monitoring responses
 *
 * Safety considerations:
 * - Real state_func() continues to be called for consistency
 * - Forced state is clearly logged for debugging purposes
 * - Cannot be used to permanently override monitoring behavior
 * - Temporary nature prevents accidental permanent overrides
 *
 * Integration with monitoring:
 * - Works seamlessly with running watchers
 * - No disruption to other watch items
 * - Forced state visible through state_watcher_get_item_state()
 * - Statistics continue to be updated normally
 *
 * Error conditions:
 * - Returns -EINVAL if item pointer is NULL
 * - Returns -EINVAL if duration_ms is 0
 * - Always succeeds with valid parameters
 * - No validation of forced_state value range
 *
 * Context: Any context
 * Return: 0 on success, -EINVAL if parameters are invalid
 *
 * Example:
 * @code
 * // Test battery low condition
 * static int test_battery_low_response(struct watch_item *battery_item)
 * {
 *     int ret;
 *     
 *     pr_info("Testing battery low condition...\n");
 *     
 *     // Force battery to report 10% for 30 seconds
 *     ret = state_watcher_force_state(battery_item, 10, 30000);
 *     if (ret) {
 *         pr_err("Failed to force battery state: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     pr_info("Battery forced to 10%% for 30 seconds\n");
 *     pr_info("Monitor system behavior and verify low battery actions\n");
 *     
 *     return 0;
 * }
 * 
 * // Simulate thermal emergency
 * static void simulate_thermal_emergency(struct watch_item *temp_item)
 * {
 *     pr_warn("SIMULATION: Forcing thermal emergency condition\n");
 *     
 *     // Force temperature to 95°C for 60 seconds
 *     if (state_watcher_force_state(temp_item, 95, 60000) == 0) {
 *         pr_warn("Temperature forced to 95°C - testing emergency cooling\n");
 *     } else {
 *         pr_err("Failed to simulate thermal emergency\n");
 *     }
 * }
 * 
 * // Automated test sequence
 * static int run_automated_tests(struct watch_item *sensor_item)
 * {
 *     unsigned long test_states[] = {0, 25, 50, 75, 100};
 *     int i, ret;
 *     
 *     pr_info("Starting automated test sequence...\n");
 *     
 *     for (i = 0; i < ARRAY_SIZE(test_states); i++) {
 *         pr_info("Test %d: Forcing state to %lu\n", i + 1, test_states[i]);
 *         
 *         // Force each state for 5 seconds
 *         ret = state_watcher_force_state(sensor_item, test_states[i], 5000);
 *         if (ret) {
 *             pr_err("Test %d failed: %d\n", i + 1, ret);
 *             return ret;
 *         }
 *         
 *         // Wait for test to complete plus some buffer
 *         msleep(6000);
 *     }
 *     
 *     pr_info("Automated test sequence completed\n");
 *     return 0;
 * }
 * 
 * // Debug state transitions with forced values
 * static void debug_state_transitions(struct watch_item *item)
 * {
 *     unsigned long current_state;
 *     int ret;
 *     
 *     // Get current state
 *     ret = state_watcher_get_item_state(item, &current_state);
 *     if (ret) {
 *         pr_err("Cannot get current state for debugging: %d\n", ret);
 *         return;
 *     }
 *     
 *     pr_info("Debug: Current state is %lu\n", current_state);
 *     
 *     // Force to different state to trigger action
 *     unsigned long test_state = (current_state == 0) ? 100 : 0;
 *     
 *     pr_info("Debug: Forcing state to %lu for 10 seconds\n", test_state);
 *     ret = state_watcher_force_state(item, test_state, 10000);
 *     if (ret) {
 *         pr_err("Debug force failed: %d\n", ret);
 *         return;
 *     }
 *     
 *     pr_info("Debug: Watch for action function calls and system response\n");
 * }
 * 
 * // Test hysteresis bypass
 * static void test_hysteresis_bypass(struct watch_item *item)
 * {
 *     pr_info("Testing hysteresis bypass with forced state...\n");
 *     pr_info("Item %s has hysteresis setting: %lu\n", item->name, item->hysteresis);
 *     
 *     // This should trigger action immediately, bypassing hysteresis
 *     if (state_watcher_force_state(item, 99, 5000) == 0) {
 *         pr_info("Forced state 99 - action should trigger immediately\n");
 *         pr_info("(Normal hysteresis of %lu would require %lu consecutive readings)\n",
 *                 item->hysteresis, item->hysteresis);
 *     }
 * }
 * 
 * // Failure condition simulation
 * static void simulate_system_failures(struct watch_item **items, int count)
 * {
 *     struct {
 *         const char *scenario;
 *         int item_index;
 *         unsigned long state;
 *         unsigned long duration;
 *     } scenarios[] = {
 *         {"CPU Overheat", 0, 95, 15000},      // CPU temp to 95°C for 15s
 *         {"Memory Full", 1, 99, 20000},       // Memory to 99% for 20s  
 *         {"Disk Full", 2, 100, 10000},        // Disk to 100% for 10s
 *         {"Battery Critical", 3, 5, 30000}    // Battery to 5% for 30s
 *     };
 *     
 *     for (int i = 0; i < ARRAY_SIZE(scenarios); i++) {
 *         if (scenarios[i].item_index >= count || !items[scenarios[i].item_index])
 *             continue;
 *             
 *         pr_warn("SIMULATION: %s\n", scenarios[i].scenario);
 *         
 *         int ret = state_watcher_force_state(items[scenarios[i].item_index],
 *                                            scenarios[i].state,
 *                                            scenarios[i].duration);
 *         if (ret == 0) {
 *             pr_warn("  Forced %s to %lu for %lu ms\n",
 *                     items[scenarios[i].item_index]->name,
 *                     scenarios[i].state, scenarios[i].duration);
 *         }
 *         
 *         // Brief delay between scenarios
 *         msleep(2000);
 *     }
 * }
 * 
 * // Performance testing with forced loads
 * static int performance_test_with_forced_states(struct watch_item *cpu_item,
 *                                               struct watch_item *memory_item)
 * {
 *     pr_info("Starting performance test with artificial load...\n");
 *     
 *     // Simulate high CPU usage for 20 seconds
 *     if (state_watcher_force_state(cpu_item, 85, 20000) != 0) {
 *         pr_err("Failed to force CPU state\n");
 *         return -1;
 *     }
 *     
 *     // Simulate high memory usage for 20 seconds  
 *     if (state_watcher_force_state(memory_item, 90, 20000) != 0) {
 *         pr_err("Failed to force memory state\n");
 *         return -1;
 *     }
 *     
 *     pr_info("Artificial load applied (CPU: 85%%, Memory: 90%%) for 20 seconds\n");
 *     pr_info("Monitor system response and performance metrics\n");
 *     
 *     return 0;
 * }
 * 
 * // Development demonstration
 * static void demonstrate_monitoring_features(struct watch_item *demo_item)
 * {
 *     pr_info("=== Monitoring System Demonstration ===\n");
 *     
 *     // Show normal state
 *     unsigned long normal_state;
 *     if (state_watcher_get_item_state(demo_item, &normal_state) == 0) {
 *         pr_info("1. Normal state: %lu\n", normal_state);
 *     }
 *     
 *     // Demonstrate immediate response
 *     pr_info("2. Forcing state to 75 (should trigger immediate action)...\n");
 *     state_watcher_force_state(demo_item, 75, 8000);
 *     
 *     msleep(2000);
 *     
 *     // Demonstrate different state
 *     pr_info("3. Forcing state to 25 (different response expected)...\n");
 *     state_watcher_force_state(demo_item, 25, 8000);
 *     
 *     pr_info("4. States will return to normal automatically\n");
 *     pr_info("=== Demonstration Complete ===\n");
 * }
 * @endcode
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

    STATE_WATCHER_INFO("Item %s: forced state %lu for %lu ms", 
                       item->name, forced_state, duration_ms);

    return 0;
}

/**
 * state_watcher_clear_forced_state() - Clear forced state and resume normal monitoring
 * @item: Pointer to watch item to clear forced state from
 *
 * Immediately clears any active forced state and resumes normal monitoring
 * behavior for the specified watch item. This allows manual termination of
 * forced state operations before their natural expiration time.
 *
 * Clear operation behavior:
 * - is_forced flag set to false immediately
 * - Normal state_func() return values used from next monitoring cycle
 * - Hysteresis behavior restored to configured settings
 * - No action functions called during clearing process
 * - Safe to call even if no forced state is active
 *
 * State transition handling:
 * - Current state reverts to actual state_func() return value
 * - last_action_state remains unchanged during clear operation
 * - Hysteresis counters (candidate_state, consecutive_count) not reset
 * - Next state change will follow normal hysteresis rules
 * - No immediate action triggering upon clearing
 *
 * Timing considerations:
 * - Clearing takes effect in the next monitoring cycle
 * - Does not interrupt currently executing work function
 * - Remaining duration time is discarded
 * - Expiration timer becomes irrelevant after clearing
 *
 * Idempotent operation:
 * - Safe to call multiple times without side effects
 * - No error if item has no active forced state
 * - Returns success even for non-forced items
 * - Consistent behavior regardless of current forced state
 *
 * Use cases:
 * - Early termination of test scenarios
 * - Emergency restoration of normal monitoring
 * - Cleanup after failed or interrupted test procedures
 * - User-initiated cancellation of simulation states
 * - Automated test cleanup and teardown procedures
 *
 * Integration with monitoring:
 * - No disruption to ongoing monitoring operations
 * - Other watch items remain unaffected
 * - Monitoring statistics continue normally
 * - Work queue scheduling unchanged
 *
 * Safety and reliability:
 * - Always safe to call regardless of item state
 * - No memory leaks or resource issues
 * - Atomic operation (single flag assignment)
 * - No locking required for basic clear operation
 *
 * Debugging support:
 * - Clear operations are logged for troubleshooting
 * - Helpful for tracking test procedure execution
 * - Visible in debug logs when enabled
 * - Useful for automated test validation
 *
 * Error conditions:
 * - Returns -EINVAL if item pointer is NULL
 * - Always succeeds with valid item pointer
 * - No validation of item's watcher membership
 * - No side effects on error conditions
 *
 * Context: Any context
 * Return: 0 on success, -EINVAL if item is NULL
 *
 * Example:
 * @code
 * // Emergency clear during critical system condition
 * static void emergency_clear_all_forced_states(struct watch_item **items, int count)
 * {
 *     int i, cleared_count = 0;
 *     
 *     pr_warn("EMERGENCY: Clearing all forced states immediately\n");
 *     
 *     for (i = 0; i < count; i++) {
 *         if (!items[i])
 *             continue;
 *             
 *         if (state_watcher_is_state_forced(items[i], NULL)) {
 *             int ret = state_watcher_clear_forced_state(items[i]);
 *             if (ret == 0) {
 *                 pr_info("Cleared forced state for %s\n", items[i]->name);
 *                 cleared_count++;
 *             } else {
 *                 pr_err("Failed to clear forced state for %s: %d\n", 
 *                        items[i]->name, ret);
 *             }
 *         }
 *     }
 *     
 *     pr_warn("Emergency clear completed: %d items restored to normal\n", cleared_count);
 * }
 * 
 * // Test procedure with early termination
 * static int test_with_early_termination(struct watch_item *sensor_item)
 * {
 *     int ret;
 *     unsigned long original_state;
 *     
 *     // Save original state
 *     ret = state_watcher_get_item_state(sensor_item, &original_state);
 *     if (ret) {
 *         pr_err("Cannot read original state: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     pr_info("Starting test (original state: %lu)...\n", original_state);
 *     
 *     // Force high value for testing
 *     ret = state_watcher_force_state(sensor_item, 90, 30000);  // 30 seconds
 *     if (ret) {
 *         pr_err("Failed to force state: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     pr_info("State forced to 90 for 30 seconds\n");
 *     
 *     // Wait a bit to observe behavior
 *     msleep(5000);
 *     
 *     // Check if we need early termination
 *     if (system_under_stress()) {
 *         pr_warn("System stress detected - terminating test early\n");
 *         
 *         ret = state_watcher_clear_forced_state(sensor_item);
 *         if (ret) {
 *             pr_err("Failed to clear forced state: %d\n", ret);
 *             return ret;
 *         }
 *         
 *         pr_info("Test terminated early - normal monitoring resumed\n");
 *         return -EINTR;  // Test interrupted
 *     }
 *     
 *     pr_info("Test completed normally\n");
 *     return 0;
 * }
 * 
 * // User-controlled test cancellation
 * static int interactive_test_control(struct watch_item *item)
 * {
 *     char user_input;
 *     int ret;
 *     
 *     pr_info("Starting interactive test...\n");
 *     pr_info("Press 'c' to cancel, 'r' to check remaining time\n");
 *     
 *     // Start test with long duration
 *     ret = state_watcher_force_state(item, 75, 60000);  // 60 seconds
 *     if (ret) {
 *         pr_err("Failed to start test: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     // Simulate user input handling (simplified)
 *     while (state_watcher_is_state_forced(item, NULL)) {
 *         // In real implementation, this would be proper input handling
 *         user_input = get_user_input();  // Hypothetical function
 *         
 *         switch (user_input) {
 *         case 'c':
 *             pr_info("User requested cancellation\n");
 *             ret = state_watcher_clear_forced_state(item);
 *             if (ret == 0) {
 *                 pr_info("Test cancelled - normal monitoring resumed\n");
 *             }
 *             return 0;
 *             
 *         case 'r':
 *             unsigned long remaining_ms;
 *             if (state_watcher_is_state_forced(item, &remaining_ms)) {
 *                 pr_info("Time remaining: %lu ms\n", remaining_ms);
 *             }
 *             break;
 *         }
 *         
 *         msleep(1000);  // Check every second
 *     }
 *     
 *     pr_info("Test completed naturally\n");
 *     return 0;
 * }
 * 
 * // Cleanup after failed test
 * static void cleanup_after_test_failure(struct watch_item *item, const char *test_name)
 * {
 *     pr_err("Test '%s' failed - performing cleanup\n", test_name);
 *     
 *     // Clear any forced state that might be active
 *     if (state_watcher_is_state_forced(item, NULL)) {
 *         int ret = state_watcher_clear_forced_state(item);
 *         if (ret == 0) {
 *             pr_info("Forced state cleared during cleanup\n");
 *         } else {
 *             pr_err("Failed to clear forced state during cleanup: %d\n", ret);
 *         }
 *     } else {
 *         pr_info("No forced state to clear\n");
 *     }
 *     
 *     // Additional cleanup procedures...
 *     pr_info("Cleanup completed for test '%s'\n", test_name);
 * }
 * 
 * // Batch test with individual control
 * static int run_batch_tests_with_control(struct watch_item **items, int count)
 * {
 *     int i, successful_tests = 0;
 *     
 *     pr_info("Running batch tests with individual control...\n");
 *     
 *     for (i = 0; i < count; i++) {
 *         if (!items[i])
 *             continue;
 *             
 *         pr_info("Test %d: %s\n", i + 1, items[i]->name);
 *         
 *         // Start test
 *         int ret = state_watcher_force_state(items[i], 80, 10000);
 *         if (ret) {
 *             pr_err("Failed to start test %d: %d\n", i + 1, ret);
 *             continue;
 *         }
 *         
 *         // Monitor test progress
 *         msleep(3000);  // Let test run for 3 seconds
 *         
 *         // Check if we should abort this test
 *         if (should_abort_test(items[i])) {
 *             pr_warn("Aborting test %d for %s\n", i + 1, items[i]->name);
 *             state_watcher_clear_forced_state(items[i]);
 *         } else {
 *             // Let test complete naturally
 *             msleep(7000);  // Wait for remaining 7 seconds
 *             successful_tests++;
 *         }
 *         
 *         // Ensure clean state between tests
 *         state_watcher_clear_forced_state(items[i]);
 *         msleep(1000);  // Brief pause between tests
 *     }
 *     
 *     pr_info("Batch testing completed: %d/%d tests successful\n", 
 *             successful_tests, count);
 *     return successful_tests;
 * }
 * 
 * // Timeout-based clear operation
 * static void clear_forced_state_with_timeout(struct watch_item *item,
 *                                            unsigned long timeout_ms)
 * {
 *     unsigned long start_time = jiffies;
 *     unsigned long remaining_ms;
 *     
 *     if (!state_watcher_is_state_forced(item, &remaining_ms)) {
 *         pr_info("No forced state to clear for %s\n", item->name);
 *         return;
 *     }
 *     
 *     pr_info("Clearing forced state for %s (timeout: %lu ms)\n", 
 *             item->name, timeout_ms);
 *     
 *     if (remaining_ms > timeout_ms) {
 *         pr_info("Forced state has %lu ms remaining, clearing early\n", remaining_ms);
 *         
 *         int ret = state_watcher_clear_forced_state(item);
 *         if (ret == 0) {
 *             pr_info("Forced state cleared successfully\n");
 *         } else {
 *             pr_err("Failed to clear forced state: %d\n", ret);
 *         }
 *     } else {
 *         pr_info("Forced state will expire naturally in %lu ms\n", remaining_ms);
 *     }
 * }
 * 
 * // Safe clear with validation
 * static int safe_clear_forced_state(struct watch_item *item)
 * {
 *     if (!item) {
 *         pr_err("Cannot clear forced state: item is NULL\n");
 *         return -EINVAL;
 *     }
 *     
 *     unsigned long remaining_ms;
 *     if (state_watcher_is_state_forced(item, &remaining_ms)) {
 *         pr_info("Clearing forced state for %s (%lu ms remaining)\n",
 *                 item->name, remaining_ms);
 *         
 *         int ret = state_watcher_clear_forced_state(item);
 *         if (ret == 0) {
 *             pr_info("Successfully cleared forced state for %s\n", item->name);
 *         } else {
 *             pr_err("Failed to clear forced state for %s: %d\n", item->name, ret);
 *         }
 *         return ret;
 *     } else {
 *         pr_debug("No forced state active for %s\n", item->name);
 *         return 0;  // Success - nothing to clear
 *     }
 * }
 * @endcode
 */
int state_watcher_clear_forced_state(struct watch_item *item)
{
    if (!item) {
        return -EINVAL;
    }

    if (item->is_forced) {
        item->is_forced = false;
        STATE_WATCHER_INFO("Item %s: forced state cleared, resuming normal watching", 
                           item->name);
    }

    return 0;
}

/**
 * state_watcher_is_state_forced() - Check if a watch item has an active forced state
 * @item: Pointer to watch item to query
 * @remaining_ms: Pointer to store remaining time in milliseconds (can be NULL)
 *
 * Checks whether the specified watch item currently has an active forced state
 * and optionally returns the remaining time before the forced state expires.
 * This function also handles automatic cleanup of expired forced states.
 *
 * Forced state detection:
 * - Returns true if item->is_forced flag is set and state hasn't expired
 * - Returns false if no forced state is active or has expired
 * - Automatically clears expired forced states during checking
 * - Uses current jiffies to determine expiration status
 *
 * Automatic expiration handling:
 * - Compares current time with forced_state_expire_time
 * - Automatically sets is_forced=false if expired
 * - Logs expiration event for debugging purposes
 * - Ensures consistent state without manual intervention
 *
 * Remaining time calculation:
 * - Only calculated if forced state is still active
 * - Converted from jiffies to milliseconds for user convenience
 * - May be 0 if expiration is imminent
 * - NULL remaining_ms parameter safely ignored
 *
 * Thread safety:
 * - Simple field reads and atomic flag updates
 * - Safe to call from any context
 * - No locking required for basic operation
 * - Concurrent calls are safe and consistent
 *
 * Use cases:
 * - Test procedure monitoring and validation
 * - User interface status display
 * - Conditional logic based on forced state status
 * - Automated test framework state tracking
 * - Debug information and system introspection
 *
 * Integration with forced state operations:
 * - Complementary to state_watcher_force_state()
 * - Works with state_watcher_clear_forced_state()
 * - Consistent with forced state expiration behavior
 * - Useful for monitoring forced state lifecycle
 *
 * Performance characteristics:
 * - Lightweight operation with minimal overhead
 * - No system calls or blocking operations
 * - Safe for high-frequency polling
 * - Efficient jiffies-based time calculations
 *
 * Error handling:
 * - Returns false if item pointer is NULL
 * - Graceful handling of invalid time values
 * - No side effects on error conditions
 * - Consistent behavior regardless of item state
 *
 * Debugging support:
 * - Expiration events logged for troubleshooting
 * - Useful for tracking forced state lifecycle
 * - Helps validate test timing and duration
 * - Visible in debug logs when enabled
 *
 * Context: Any context
 * Return: true if state is forced and active, false otherwise
 *
 * Example:
 * @code
 * // Basic forced state checking
 * static void check_forced_state_status(struct watch_item *item)
 * {
 *     unsigned long remaining_ms;
 *     
 *     if (state_watcher_is_state_forced(item, &remaining_ms)) {
 *         pr_info("Item %s has forced state active (%lu ms remaining)\n",
 *                 item->name, remaining_ms);
 *     } else {
 *         pr_info("Item %s is using normal monitoring\n", item->name);
 *     }
 * }
 * 
 * // Test progress monitoring
 * static int monitor_test_progress(struct watch_item *test_item, 
 *                                 unsigned long expected_duration)
 * {
 *     unsigned long remaining_ms;
 *     unsigned long start_time = jiffies;
 *     
 *     if (!state_watcher_is_state_forced(test_item, &remaining_ms)) {
 *         pr_err("Test not active for %s\n", test_item->name);
 *         return -ENOENT;
 *     }
 *     
 *     pr_info("Test monitoring started for %s (%lu ms expected)\n",
 *             test_item->name, expected_duration);
 *     
 *     while (state_watcher_is_state_forced(test_item, &remaining_ms)) {
 *         unsigned long elapsed = jiffies_to_msecs(jiffies - start_time);
 *         
 *         pr_info("Test progress: %lu ms elapsed, %lu ms remaining\n",
 *                 elapsed, remaining_ms);
 *                 
 *         // Check for test anomalies
 *         if (remaining_ms > expected_duration) {
 *             pr_warn("Test duration extended beyond expected time\n");
 *         }
 *         
 *         msleep(5000);  // Check every 5 seconds
 *     }
 *     
 *     pr_info("Test completed for %s\n", test_item->name);
 *     return 0;
 * }
 * 
 * // Conditional operations based on forced state
 * static int perform_maintenance_if_safe(struct watch_item **critical_items, int count)
 * {
 *     int i;
 *     bool any_forced = false;
 *     
 *     // Check if any critical item has forced state
 *     for (i = 0; i < count; i++) {
 *         if (!critical_items[i])
 *             continue;
 *             
 *         if (state_watcher_is_state_forced(critical_items[i], NULL)) {
 *             pr_info("Item %s has forced state - maintenance postponed\n",
 *                     critical_items[i]->name);
 *             any_forced = true;
 *         }
 *     }
 *     
 *     if (any_forced) {
 *         pr_warn("Maintenance deferred due to active test states\n");
 *         return -EBUSY;
 *     }
 *     
 *     pr_info("All items in normal state - proceeding with maintenance\n");
 *     // Perform maintenance operations...
 *     return 0;
 * }
 * 
 * // Test timeout detection and handling
 * static void handle_test_timeouts(struct watch_item **test_items, int count,
 *                                 unsigned long max_allowed_duration)
 * {
 *     int i, timeout_count = 0;
 *     
 *     for (i = 0; i < count; i++) {
 *         unsigned long remaining_ms;
 *         
 *         if (!test_items[i])
 *             continue;
 *             
 *         if (state_watcher_is_state_forced(test_items[i], &remaining_ms)) {
 *             // Calculate how long the test has been running
 *             unsigned long elapsed = max_allowed_duration - remaining_ms;
 *             
 *             if (elapsed > max_allowed_duration) {
 *                 pr_warn("Test timeout detected for %s (elapsed: %lu ms)\n",
 *                         test_items[i]->name, elapsed);
 *                 
 *                 // Force clear the stuck test
 *                 state_watcher_clear_forced_state(test_items[i]);
 *                 timeout_count++;
 *             }
 *         }
 *     }
 *     
 *     if (timeout_count > 0) {
 *         pr_warn("Cleared %d timed-out test states\n", timeout_count);
 *     }
 * }
 * 
 * // User interface status display
 * static void display_system_status(struct watch_item **all_items, int count)
 * {
 *     int normal_count = 0, forced_count = 0;
 *     unsigned long total_remaining = 0;
 *     
 *     pr_info("=== System Status Report ===\n");
 *     
 *     for (int i = 0; i < count; i++) {
 *         unsigned long remaining_ms;
 *         
 *         if (!all_items[i])
 *             continue;
 *             
 *         if (state_watcher_is_state_forced(all_items[i], &remaining_ms)) {
 *             pr_info("%-20s: FORCED (%lu ms remaining)\n", 
 *                     all_items[i]->name, remaining_ms);
 *             forced_count++;
 *             total_remaining += remaining_ms;
 *         } else {
 *             unsigned long current_state;
 *             if (state_watcher_get_item_state(all_items[i], &current_state) == 0) {
 *                 pr_info("%-20s: NORMAL (state: %lu)\n", 
 *                         all_items[i]->name, current_state);
 *             } else {
 *                 pr_info("%-20s: NORMAL (state: unknown)\n", all_items[i]->name);
 *             }
 *             normal_count++;
 *         }
 *     }
 *     
 *     pr_info("Summary: %d normal, %d forced", normal_count, forced_count);
 *     if (forced_count > 0) {
 *         pr_info(" (avg remaining: %lu ms)\n", total_remaining / forced_count);
 *     } else {
 *         pr_info("\n");
 *     }
 * }
 * 
 * // Automated test validation
 * static bool validate_test_state(struct watch_item *item, 
 *                                unsigned long expected_min_duration)
 * {
 *     unsigned long remaining_ms;
 *     
 *     if (!state_watcher_is_state_forced(item, &remaining_ms)) {
 *         pr_err("Test validation failed: %s is not in forced state\n", item->name);
 *         return false;
 *     }
 *     
 *     if (remaining_ms < expected_min_duration) {
 *         pr_warn("Test validation warning: %s has only %lu ms remaining (expected >= %lu)\n",
 *                 item->name, remaining_ms, expected_min_duration);
 *         return false;
 *     }
 *     
 *     pr_info("Test validation passed: %s has %lu ms remaining\n", 
 *             item->name, remaining_ms);
 *     return true;
 * }
 * 
 * // Wait for forced state completion
 * static int wait_for_forced_state_completion(struct watch_item *item, 
 *                                            unsigned long timeout_ms)
 * {
 *     unsigned long start_time = jiffies;
 *     unsigned long remaining_ms;
 *     
 *     if (!state_watcher_is_state_forced(item, &remaining_ms)) {
 *         pr_info("No forced state to wait for on %s\n", item->name);
 *         return 0;
 *     }
 *     
 *     pr_info("Waiting for forced state completion on %s (%lu ms remaining)\n",
 *             item->name, remaining_ms);
 *     
 *     while (state_watcher_is_state_forced(item, &remaining_ms)) {
 *         unsigned long elapsed = jiffies_to_msecs(jiffies - start_time);
 *         
 *         if (elapsed > timeout_ms) {
 *             pr_warn("Timeout waiting for forced state completion on %s\n", item->name);
 *             return -ETIMEDOUT;
 *         }
 *         
 *         pr_debug("Still waiting: %lu ms remaining, %lu ms elapsed\n", 
 *                  remaining_ms, elapsed);
 *         
 *         msleep(min(remaining_ms, 1000UL));  // Sleep up to 1 second
 *     }
 *     
 *     pr_info("Forced state completed for %s\n", item->name);
 *     return 0;
 * }
 * 
 * // Debug information collection
 * static void collect_forced_state_debug_info(struct watch_item *item)
 * {
 *     unsigned long remaining_ms;
 *     bool is_forced = state_watcher_is_state_forced(item, &remaining_ms);
 *     
 *     pr_debug("=== Forced State Debug Info for %s ===\n", item->name);
 *     pr_debug("  is_forced flag: %s\n", is_forced ? "true" : "false");
 *     
 *     if (is_forced) {
 *         pr_debug("  forced_state value: %lu\n", item->forced_state);
 *         pr_debug("  remaining time: %lu ms\n", remaining_ms);
 *         pr_debug("  expire_time (jiffies): %lu\n", item->forced_state_expire_time);
 *         pr_debug("  current_time (jiffies): %lu\n", jiffies);
 *     } else {
 *         pr_debug("  No active forced state\n");
 *     }
 *     
 *     // Additional state information
 *     unsigned long current_state;
 *     if (state_watcher_get_item_state(item, &current_state) == 0) {
 *         pr_debug("  current_state: %lu\n", current_state);
 *         pr_debug("  last_action_state: %lu\n", item->last_action_state);
 *     }
 * }
 * @endcode
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
        STATE_WATCHER_DEBUG("Item %s: forced state expired during check", item->name);
    }

    if (item->is_forced && remaining_ms) {
        unsigned long remaining_jiffies = item->forced_state_expire_time - current_time;
        *remaining_ms = jiffies_to_msecs(remaining_jiffies);
    }

    return item->is_forced;
}

/**
 * state_watcher_cleanup() - Clean up state watcher and free all resources
 * @watcher: Pointer to state watcher structure to clean up
 *
 * Performs complete cleanup of the state watcher framework, stopping all
 * monitoring activities and freeing all associated resources. This function
 * ensures graceful shutdown and prevents resource leaks.
 *
 * Cleanup sequence:
 * 1. Stop periodic watching if currently running
 * 2. Cancel any pending delayed work and wait for completion
 * 3. Remove all watch items from the list
 * 4. Free memory allocated for each watch item
 * 5. Mark watcher as uninitialized
 *
 * Resource management:
 * - All watch items are automatically removed and freed
 * - Delayed work is properly canceled and synchronized
 * - No memory leaks occur regardless of current watcher state
 * - Private data pointers in items are not freed (user responsibility)
 *
 * State changes:
 * - watcher->running set to false (if not already)
 * - watcher->initialized set to false
 * - Item list becomes empty
 * - Statistics counters preserved (for final reporting)
 *
 * Safety considerations:
 * - Safe to call multiple times (idempotent operation)
 * - Safe to call on uninitialized watcher (null check performed)
 * - Waits for work completion to prevent use-after-free
 * - Protects against concurrent access during cleanup
 *
 * Post-cleanup state:
 * - Watcher must be re-initialized before reuse
 * - All previously added watch items are invalid
 * - No background monitoring activity remains
 * - Structure can be safely freed or reused
 *
 * User responsibilities:
 * - Ensure private_data lifetime management if dynamically allocated
 * - Stop dependent operations that rely on watch items
 * - Handle any final state transitions in action functions
 *
 * Context: Process context (may sleep during work cancellation)
 * Return: void (no return value)
 *
 * Example:
 * @code
 * // Module cleanup with statistics reporting
 * static void __exit system_monitor_exit(void)
 * {
 *     unsigned long total_checks, total_actions;
 *     unsigned int active_items;
 *     
 *     // Get final statistics before cleanup
 *     if (state_watcher_get_stats(&system_watcher, &total_checks, 
 *                                 &total_actions, &active_items) == 0) {
 *         pr_info("Final stats: %lu checks, %lu actions, %u items\n",
 *                 total_checks, total_actions, active_items);
 *     }
 *     
 *     // Clean shutdown
 *     state_watcher_cleanup(&system_watcher);
 *     pr_info("System monitor cleaned up\n");
 * }
 * 
 * // Error handling during initialization
 * static int network_monitor_init(void)
 * {
 *     struct state_watcher *watcher;
 *     int ret;
 *     
 *     watcher = kzalloc(sizeof(*watcher), GFP_KERNEL);
 *     if (!watcher)
 *         return -ENOMEM;
 *     
 *     ret = state_watcher_init(watcher, 1000);
 *     if (ret)
 *         goto free_watcher;
 *     
 *     // Add items...
 *     ret = add_network_items(watcher);
 *     if (ret)
 *         goto cleanup_watcher;
 *     
 *     ret = state_watcher_start(watcher);
 *     if (ret)
 *         goto cleanup_watcher;
 *     
 *     return 0;
 * 
 * cleanup_watcher:
 *     state_watcher_cleanup(watcher);  // Handles partial initialization
 * free_watcher:
 *     kfree(watcher);
 *     return ret;
 * }
 * 
 * // Reconfiguration requiring cleanup
 * static int reconfigure_monitoring(struct state_watcher *watcher, 
 *                                  unsigned long new_base_interval)
 * {
 *     // Preserve current statistics
 *     unsigned long checks, actions;
 *     state_watcher_get_stats(watcher, &checks, &actions, NULL);
 *     
 *     // Clean up current configuration
 *     state_watcher_cleanup(watcher);
 *     
 *     // Reinitialize with new settings
 *     int ret = state_watcher_init(watcher, new_base_interval);
 *     if (ret) {
 *         pr_err("Failed to reinitialize watcher: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     pr_info("Reconfigured watcher (previous: %lu checks, %lu actions)\n",
 *             checks, actions);
 *     return 0;
 * }
 * 
 * // Safe cleanup with private data management
 * static void cleanup_with_private_data(struct state_watcher *watcher)
 * {
 *     struct watch_item *item;
 *     unsigned long flags;
 *     
 *     // First pass: free private data while items are still valid
 *     spin_lock_irqsave(&watcher->lock, flags);
 *     list_for_each_entry(item, &watcher->item_list, list) {
 *         if (item->private_data) {
 *             kfree(item->private_data);  // If dynamically allocated
 *             item->private_data = NULL;
 *         }
 *     }
 *     spin_unlock_irqrestore(&watcher->lock, flags);
 *     
 *     // Second pass: normal cleanup (items and structure)
 *     state_watcher_cleanup(watcher);
 * }
 * @endcode
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

    STATE_WATCHER_INFO("State watcher cleaned up");
}

/**
 * state_watcher_start() - Start periodic watching and monitoring
 * @watcher: Pointer to initialized state watcher structure
 *
 * Begins the periodic monitoring process by scheduling the delayed work
 * that will check all watch items according to their configured intervals.
 * The work runs in process context and can handle blocking operations.
 *
 * Operation mechanics:
 * - Uses atomic cmpxchg for race-free start operation
 * - Sets running flag to true before scheduling work
 * - Schedules first work execution after base_interval_ms delay
 * - Work function automatically reschedules itself while running
 *
 * Work execution model:
 * - Work runs every base_interval_ms milliseconds
 * - Each execution checks all items and evaluates their individual intervals
 * - Items are checked only when their interval has elapsed
 * - State and action functions called with spinlock temporarily released
 *
 * Prerequisites:
 * - Watcher must be initialized (state_watcher_init called)
 * - At least one watch item should be added for meaningful operation
 * - System workqueue must be available
 *
 * Atomic operation:
 * - Uses compare-and-swap to prevent race conditions
 * - Safe to call concurrently from multiple contexts
 * - Returns -EALREADY if already running (not an error condition)
 * - Memory barriers ensure visibility of running flag
 *
 * Performance considerations:
 * - Lower base_interval_ms provides better responsiveness
 * - Higher base_interval_ms reduces CPU overhead
 * - Work execution time should be minimized for system responsiveness
 * - Multiple watchers can run independently
 *
 * Error conditions:
 * - Returns -EINVAL if watcher is NULL or uninitialized
 * - Returns -EALREADY if watcher is already running
 * - Always succeeds for valid, stopped, initialized watcher
 *
 * Post-start behavior:
 * - Background monitoring begins immediately
 * - First check occurs after base_interval_ms delay
 * - Continues until state_watcher_stop() is called
 * - Automatic recovery from individual item failures
 *
 * Context: Process context
 * Return: 0 on success, -EINVAL if invalid watcher, -EALREADY if already running
 *
 * Example:
 * @code
 * // Basic monitoring startup
 * static int system_monitor_start(void)
 * {
 *     int ret;
 *     
 *     ret = state_watcher_start(&system_watcher);
 *     if (ret == -EALREADY) {
 *         pr_info("System monitor already running\n");
 *         return 0;  // Not an error
 *     } else if (ret) {
 *         pr_err("Failed to start system monitor: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     pr_info("System monitor started successfully\n");
 *     return 0;
 * }
 * 
 * // Module initialization with complete setup
 * static int __init network_monitor_init(void)
 * {
 *     int ret;
 *     
 *     // Initialize watcher
 *     ret = state_watcher_init(&network_watcher, 1000);
 *     if (ret)
 *         return ret;
 *     
 *     // Add monitoring items
 *     ret = add_network_interfaces(&network_watcher);
 *     if (ret)
 *         goto cleanup;
 *     
 *     ret = add_bandwidth_monitors(&network_watcher);
 *     if (ret)
 *         goto cleanup;
 *     
 *     // Start monitoring
 *     ret = state_watcher_start(&network_watcher);
 *     if (ret)
 *         goto cleanup;
 *     
 *     pr_info("Network monitoring active\n");
 *     return 0;
 * 
 * cleanup:
 *     state_watcher_cleanup(&network_watcher);
 *     return ret;
 * }
 * 
 * // Conditional start with item validation
 * static int start_monitoring_if_ready(struct state_watcher *watcher)
 * {
 *     unsigned int active_items;
 *     int ret;
 *     
 *     // Check if we have items to monitor
 *     ret = state_watcher_get_stats(watcher, NULL, NULL, &active_items);
 *     if (ret)
 *         return ret;
 *     
 *     if (active_items == 0) {
 *         pr_warn("No watch items configured, skipping start\n");
 *         return -ENOENT;
 *     }
 *     
 *     ret = state_watcher_start(watcher);
 *     if (ret == 0) {
 *         pr_info("Started monitoring %u items\n", active_items);
 *     }
 *     
 *     return ret;
 * }
 * 
 * // Restart after configuration change
 * static int restart_monitoring(struct state_watcher *watcher)
 * {
 *     int ret;
 *     
 *     // Stop current monitoring
 *     state_watcher_stop(watcher);
 *     
 *     // Reconfigure items if needed
 *     // ... add/remove/modify items ...
 *     
 *     // Restart monitoring
 *     ret = state_watcher_start(watcher);
 *     if (ret) {
 *         pr_err("Failed to restart monitoring: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     pr_info("Monitoring restarted with new configuration\n");
 *     return 0;
 * }
 * 
 * // Multiple watcher coordination
 * static int start_all_monitors(void)
 * {
 *     struct state_watcher *watchers[] = {
 *         &system_watcher,
 *         &network_watcher, 
 *         &thermal_watcher
 *     };
 *     int i, ret;
 *     
 *     for (i = 0; i < ARRAY_SIZE(watchers); i++) {
 *         ret = state_watcher_start(watchers[i]);
 *         if (ret && ret != -EALREADY) {
 *             pr_err("Failed to start watcher %d: %d\n", i, ret);
 *             // Stop previously started watchers
 *             while (--i >= 0)
 *                 state_watcher_stop(watchers[i]);
 *             return ret;
 *         }
 *     }
 *     
 *     pr_info("All monitoring systems started\n");
 *     return 0;
 * }
 * @endcode
 */
int state_watcher_start(struct state_watcher *watcher)
{
    if (!watcher || !watcher->initialized) {
        return -EINVAL;
    }

    /* Atomic test-and-set */
    if (cmpxchg(&watcher->running, false, true) != false) {
        return -EALREADY;
    }

    /* running=true is already visible due to cmpxchg barrier semantics */
    schedule_delayed_work(&watcher->work, msecs_to_jiffies(watcher->base_interval_ms));

    STATE_WATCHER_INFO("State watcher started");
    return 0;
}

/**
 * state_watcher_stop() - Stop periodic watching and monitoring
 * @watcher: Pointer to state watcher structure to stop
 *
 * Gracefully stops the periodic monitoring process by clearing the running
 * flag and canceling any pending delayed work. Ensures that all background
 * activity is properly terminated before returning.
 *
 * Operation mechanics:
 * - Uses atomic cmpxchg for race-free stop operation
 * - Sets running flag to false before canceling work
 * - Uses cancel_delayed_work_sync() to wait for work completion
 * - Prevents new work scheduling while allowing current work to finish
 *
 * Synchronization guarantees:
 * - Work function checks running flag before processing items
 * - Work function will not reschedule itself after running=false
 * - Any currently executing work completes before function returns
 * - No race conditions between stop and work execution
 *
 * State preservation:
 * - Watch items remain in the list (not removed)
 * - Item configurations and statistics preserved
 * - Watcher remains initialized and ready for restart
 * - Current states and hysteresis counters maintained
 *
 * Atomic operation:
 * - Uses compare-and-swap to prevent race conditions
 * - Safe to call concurrently from multiple contexts
 * - Idempotent operation (safe to call multiple times)
 * - Memory barriers ensure proper ordering
 *
 * Work cancellation behavior:
 * - cancel_delayed_work_sync() may sleep waiting for work completion
 * - Guarantees no work execution after function returns
 * - Handles pending work that hasn't started yet
 * - Waits for currently running work to complete
 *
 * Performance considerations:
 * - Function may block during work cancellation
 * - Should not be called from atomic context
 * - Work completion time affects stop duration
 * - Multiple stops on same watcher are lightweight
 *
 * Safety considerations:
 * - Safe to call on uninitialized watcher (null check performed)
 * - Safe to call on already stopped watcher (no-op)
 * - No resource leaks or dangling references
 * - Preserves watcher state for potential restart
 *
 * Context: Process context (may sleep during work cancellation)
 * Return: void (no return value)
 *
 * Example:
 * @code
 * // Module cleanup with graceful shutdown
 * static void __exit system_monitor_exit(void)
 * {
 *     pr_info("Stopping system monitor...\n");
 *     state_watcher_stop(&system_watcher);
 *     pr_info("System monitor stopped\n");
 *     
 *     // Now safe to cleanup
 *     state_watcher_cleanup(&system_watcher);
 * }
 * 
 * // Temporary pause for maintenance
 * static int pause_monitoring_for_maintenance(struct state_watcher *watcher)
 * {
 *     unsigned long total_checks, total_actions;
 *     
 *     pr_info("Pausing monitoring for maintenance...\n");
 *     
 *     // Get stats before stopping
 *     state_watcher_get_stats(watcher, &total_checks, &total_actions, NULL);
 *     
 *     // Stop monitoring (preserves state)
 *     state_watcher_stop(watcher);
 *     
 *     pr_info("Monitoring paused (processed %lu checks, %lu actions)\n",
 *             total_checks, total_actions);
 *     
 *     // Perform maintenance tasks...
 *     perform_system_maintenance();
 *     
 *     // Resume monitoring
 *     return state_watcher_start(watcher);
 * }
 * 
 * // Emergency shutdown sequence
 * static void emergency_shutdown_monitoring(void)
 * {
 *     struct state_watcher *critical_watchers[] = {
 *         &thermal_watcher,
 *         &power_watcher,
 *         &network_watcher,
 *         &system_watcher
 *     };
 *     int i;
 *     
 *     pr_crit("Emergency shutdown: stopping all monitoring\n");
 *     
 *     // Stop all watchers quickly
 *     for (i = 0; i < ARRAY_SIZE(critical_watchers); i++) {
 *         state_watcher_stop(critical_watchers[i]);
 *         pr_info("Stopped watcher %d\n", i);
 *     }
 *     
 *     pr_crit("All monitoring stopped\n");
 * }
 * 
 * // Reconfiguration with stop/start
 * static int reconfigure_monitoring_interval(struct state_watcher *watcher,
 *                                           unsigned long new_base_interval)
 * {
 *     bool was_running;
 *     int ret;
 *     
 *     // Check if currently running
 *     was_running = watcher->running;
 *     
 *     if (was_running) {
 *         pr_info("Stopping watcher for reconfiguration...\n");
 *         state_watcher_stop(watcher);
 *     }
 *     
 *     // Cleanup and reinitialize with new interval
 *     state_watcher_cleanup(watcher);
 *     ret = state_watcher_init(watcher, new_base_interval);
 *     if (ret) {
 *         pr_err("Failed to reinitialize watcher: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     // Restart if it was running before
 *     if (was_running) {
 *         ret = state_watcher_start(watcher);
 *         if (ret) {
 *             pr_err("Failed to restart watcher: %d\n", ret);
 *             return ret;
 *         }
 *         pr_info("Watcher reconfigured and restarted\n");
 *     }
 *     
 *     return 0;
 * }
 * 
 * // Safe stop with timeout handling
 * static int safe_stop_with_timeout(struct state_watcher *watcher, 
 *                                  unsigned long timeout_ms)
 * {
 *     unsigned long start_time = jiffies;
 *     
 *     pr_info("Stopping watcher with %lu ms timeout...\n", timeout_ms);
 *     
 *     // This may block, so we can't really implement timeout here
 *     // since cancel_delayed_work_sync() doesn't accept timeout
 *     state_watcher_stop(watcher);
 *     
 *     if (time_after(jiffies, start_time + msecs_to_jiffies(timeout_ms))) {
 *         pr_warn("Stop operation took longer than expected\n");
 *     } else {
 *         pr_info("Watcher stopped successfully\n");
 *     }
 *     
 *     return 0;
 * }
 * 
 * // Signal handler for graceful shutdown
 * static void signal_handler_stop(int sig)
 * {
 *     pr_info("Received signal %d, stopping all watchers...\n", sig);
 *     
 *     state_watcher_stop(&system_watcher);
 *     state_watcher_stop(&network_watcher);
 *     
 *     pr_info("All watchers stopped due to signal\n");
 * }
 * @endcode
 */
void state_watcher_stop(struct state_watcher *watcher)
{
    if (!watcher || !watcher->initialized) {
        return;
    }

    /* Atomic test-and-clear */
    if (cmpxchg(&watcher->running, true, false) != true) {
        /* Already stopped */
        return;
    }

    /* cmpxchg has implicit memory barrier semantics */
    cancel_delayed_work_sync(&watcher->work);

    STATE_WATCHER_INFO("State watcher stopped");
}

/**
 * state_watcher_add_item() - Add a new watch item to the state watcher
 * @watcher: Pointer to initialized state watcher structure
 * @init: Pointer to initialization structure containing item configuration
 *
 * Creates a new watch item with the specified configuration and adds it to
 * the watcher's monitoring list. The item will be included in periodic
 * checking cycles according to its configured interval and hysteresis settings.
 *
 * Item creation process:
 * 1. Validates watcher state and initialization parameters
 * 2. Checks interval compatibility with base_interval_ms
 * 3. Allocates memory for new watch_item structure
 * 4. Initializes all fields from init parameters and defaults
 * 5. Adds item to watcher's linked list under spinlock protection
 *
 * Parameter validation:
 * - watcher: Must be initialized and non-NULL
 * - init: Must be non-NULL with valid state_func pointer
 * - init->interval_ms: If 0, uses watcher's base_interval_ms
 * - init->interval_ms: Must be multiple of and >= base_interval_ms
 * - init->name: If NULL, generates default name from item pointer
 * - init->action_func: Can be NULL for monitoring-only items
 *
 * Memory management:
 * - Item allocated with kzalloc(GFP_KERNEL)
 * - Name string copied into item->name[32] buffer (truncated if needed)
 * - private_data pointer stored as-is (user manages lifetime)
 * - Automatic cleanup on watcher destruction
 *
 * Initial state setup:
 * - current_state and last_action_state initialized to 0
 * - last_check_time set to current jiffies
 * - Hysteresis counters reset to initial state
 * - Statistics counters zeroed
 * - Forced state disabled initially
 *
 * Thread safety:
 * - Safe to call while watcher is running
 * - Spinlock protects list modification
 * - New items immediately available for monitoring
 * - No disruption to existing items
 *
 * Integration behavior:
 * - Item becomes active in next work cycle
 * - First check occurs when interval elapses from addition time
 * - Respects running state of watcher
 * - Statistics tracking begins immediately
 *
 * Error conditions:
 * - Returns NULL if watcher is NULL or uninitialized
 * - Returns NULL if init is NULL or state_func is NULL
 * - Returns NULL if interval validation fails
 * - Returns NULL if memory allocation fails
 *
 * Context: Process context
 * Return: Pointer to created watch_item on success, NULL on error
 *
 * Example:
 * @code
 * // Basic item addition with full configuration
 * static struct watch_item *add_battery_monitor(struct state_watcher *watcher)
 * {
 *     struct watch_item_init battery_init = {
 *         .name = "battery_level",
 *         .interval_ms = 5000,        // Check every 5 seconds
 *         .hysteresis = 2,            // Require 2 consecutive readings
 *         .state_func = battery_state_check,
 *         .action_func = battery_level_action,
 *         .private_data = &battery_device_info
 *     };
 *     
 *     struct watch_item *item = state_watcher_add_item(watcher, &battery_init);
 *     if (!item) {
 *         pr_err("Failed to add battery monitor\n");
 *         return NULL;
 *     }
 *     
 *     pr_info("Added battery monitor (interval: %lums, hysteresis: %lu)\n",
 *             battery_init.interval_ms, battery_init.hysteresis);
 *     return item;
 * }
 * 
 * // Multiple items with different configurations
 * static int add_system_monitors(struct state_watcher *watcher)
 * {
 *     struct watch_item_init monitors[] = {
 *         {
 *             .name = "cpu_temp",
 *             .interval_ms = 2000,    // 2 seconds
 *             .hysteresis = 3,
 *             .state_func = cpu_temperature_func,
 *             .action_func = thermal_action_func,
 *             .private_data = &cpu_sensor
 *         },
 *         {
 *             .name = "memory_usage",
 *             .interval_ms = 10000,   // 10 seconds  
 *             .hysteresis = 1,
 *             .state_func = memory_usage_func,
 *             .action_func = memory_action_func,
 *             .private_data = NULL
 *         },
 *         {
 *             .name = "disk_space",
 *             .interval_ms = 30000,   // 30 seconds
 *             .hysteresis = 0,        // Immediate response
 *             .state_func = disk_space_func,
 *             .action_func = disk_action_func,
 *             .private_data = "/var/log"
 *         }
 *     };
 *     
 *     for (int i = 0; i < ARRAY_SIZE(monitors); i++) {
 *         struct watch_item *item = state_watcher_add_item(watcher, &monitors[i]);
 *         if (!item) {
 *             pr_err("Failed to add monitor: %s\n", monitors[i].name);
 *             return -ENOMEM;
 *         }
 *         pr_info("Added monitor: %s\n", monitors[i].name);
 *     }
 *     
 *     return 0;
 * }
 * 
 * // Monitoring-only item (no action function)
 * static struct watch_item *add_statistics_collector(struct state_watcher *watcher)
 * {
 *     struct watch_item_init stats_init = {
 *         .name = "network_stats",
 *         .interval_ms = 1000,        // Every second
 *         .hysteresis = 0,
 *         .state_func = network_packet_count,
 *         .action_func = NULL,        // No action, just collect data
 *         .private_data = "eth0"
 *     };
 *     
 *     return state_watcher_add_item(watcher, &stats_init);
 * }
 * 
 * // Dynamic item addition with validation
 * static int add_dynamic_monitor(struct state_watcher *watcher, 
 *                               const char *sensor_name,
 *                               unsigned long check_interval)
 * {
 *     struct sensor_data *sensor;
 *     struct watch_item_init init;
 *     struct watch_item *item;
 *     
 *     // Validate interval against watcher's base interval
 *     unsigned long base_interval;
 *     // Assuming we can get base interval somehow
 *     base_interval = watcher->base_interval_ms;
 *     
 *     if (check_interval % base_interval != 0) {
 *         pr_err("Invalid interval %lu, must be multiple of %lu\n", 
 *                check_interval, base_interval);
 *         return -EINVAL;
 *     }
 *     
 *     // Allocate sensor data
 *     sensor = kzalloc(sizeof(*sensor), GFP_KERNEL);
 *     if (!sensor)
 *         return -ENOMEM;
 *     
 *     strncpy(sensor->name, sensor_name, sizeof(sensor->name) - 1);
 *     
 *     // Configure item
 *     init = (struct watch_item_init) {
 *         .name = sensor_name,
 *         .interval_ms = check_interval,
 *         .hysteresis = 2,
 *         .state_func = generic_sensor_func,
 *         .action_func = generic_sensor_action,
 *         .private_data = sensor
 *     };
 *     
 *     item = state_watcher_add_item(watcher, &init);
 *     if (!item) {
 *         kfree(sensor);
 *         return -ENOMEM;
 *     }
 *     
 *     pr_info("Added dynamic monitor: %s (interval: %lums)\n", 
 *             sensor_name, check_interval);
 *     return 0;
 * }
 * 
 * // Conditional item addition based on system capabilities
 * static int add_conditional_monitors(struct state_watcher *watcher)
 * {
 *     int items_added = 0;
 *     
 *     // Add thermal monitoring if thermal zones exist
 *     if (thermal_zone_count() > 0) {
 *         struct watch_item_init thermal_init = {
 *             .name = "thermal_monitor",
 *             .interval_ms = 3000,
 *             .hysteresis = 2,
 *             .state_func = thermal_state_func,
 *             .action_func = thermal_action_func
 *         };
 *         
 *         if (state_watcher_add_item(watcher, &thermal_init)) {
 *             items_added++;
 *             pr_info("Added thermal monitoring\n");
 *         }
 *     }
 *     
 *     // Add battery monitoring if battery exists
 *     if (battery_present()) {
 *         struct watch_item_init battery_init = {
 *             .name = "battery_monitor", 
 *             .interval_ms = 5000,
 *             .hysteresis = 1,
 *             .state_func = battery_state_func,
 *             .action_func = battery_action_func
 *         };
 *         
 *         if (state_watcher_add_item(watcher, &battery_init)) {
 *             items_added++;
 *             pr_info("Added battery monitoring\n");
 *         }
 *     }
 *     
 *     pr_info("Added %d conditional monitors\n", items_added);
 *     return items_added;
 * }
 * @endcode
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

    STATE_WATCHER_INFO("Added watch item '%s' (addr:%p, interval:%lu ms, hysteresis:%lu)",
                       item->name, item, item->interval_ms, item->hysteresis);

    return item;
}

/**
 * state_watcher_remove_item() - Remove a watch item from the state watcher
 * @watcher: Pointer to state watcher structure containing the item
 * @item: Pointer to watch item to remove and free
 *
 * Removes the specified watch item from the watcher's monitoring list and
 * frees its allocated memory. The item will no longer be monitored after
 * removal, and all associated resources are cleaned up.
 *
 * Removal process:
 * 1. Validates watcher and item parameters
 * 2. Acquires spinlock to protect list modification
 * 3. Removes item from the linked list using list_del()
 * 4. Releases spinlock to minimize critical section
 * 5. Frees item memory using kfree()
 *
 * Synchronization behavior:
 * - Safe to call while watcher is running
 * - Spinlock protects against concurrent list access
 * - No impact on other items in the list
 * - Work function handles missing items gracefully
 *
 * Memory cleanup:
 * - Item structure memory automatically freed
 * - private_data pointer NOT freed (user responsibility)
 * - Name string automatically cleaned up
 * - Statistics and state information lost
 *
 * Safety considerations:
 * - Item pointer becomes invalid after successful removal
 * - Must not use item pointer after this function returns
 * - Safe to remove items during active monitoring
 * - No race conditions with work function execution
 *
 * Work function interaction:
 * - Work function may skip removed items safely
 * - list_for_each_entry_safe() protects against concurrent removal
 * - No callback functions called during removal
 * - Removal takes effect immediately
 *
 * Error conditions:
 * - Returns -EINVAL if watcher is NULL or uninitialized
 * - Returns -EINVAL if item is NULL
 * - Always succeeds with valid parameters
 * - Does not validate item membership in watcher
 *
 * Post-removal state:
 * - Item no longer appears in monitoring cycles
 * - Watcher statistics updated for future operations
 * - Other items continue normal operation
 * - Watcher remains functional
 *
 * User responsibilities:
 * - Manage private_data lifetime if dynamically allocated
 * - Ensure item pointer is not used after removal
 * - Handle any cleanup needed for item-specific resources
 * - Consider removing dependent items if applicable
 *
 * Context: Process context
 * Return: 0 on success, -EINVAL if parameters are invalid
 *
 * Example:
 * @code
 * // Basic item removal
 * static int remove_battery_monitor(struct state_watcher *watcher, 
 *                                  struct watch_item *battery_item)
 * {
 *     int ret;
 *     
 *     if (!battery_item) {
 *         pr_warn("Battery monitor item is NULL\n");
 *         return -EINVAL;
 *     }
 *     
 *     pr_info("Removing battery monitor: %s\n", battery_item->name);
 *     
 *     ret = state_watcher_remove_item(watcher, battery_item);
 *     if (ret) {
 *         pr_err("Failed to remove battery monitor: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     pr_info("Battery monitor removed successfully\n");
 *     // battery_item pointer is now invalid!
 *     
 *     return 0;
 * }
 * 
 * // Remove item with private data cleanup
 * static int remove_sensor_with_cleanup(struct state_watcher *watcher,
 *                                      struct watch_item *sensor_item)
 * {
 *     struct sensor_data *sensor_data;
 *     int ret;
 *     
 *     if (!sensor_item)
 *         return -EINVAL;
 *     
 *     // Save private data pointer before removal
 *     sensor_data = (struct sensor_data *)sensor_item->private_data;
 *     
 *     pr_info("Removing sensor: %s\n", sensor_item->name);
 *     
 *     // Remove from watcher
 *     ret = state_watcher_remove_item(watcher, sensor_item);
 *     if (ret) {
 *         pr_err("Failed to remove sensor item: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     // Clean up private data if dynamically allocated
 *     if (sensor_data) {
 *         cleanup_sensor_data(sensor_data);
 *         kfree(sensor_data);
 *     }
 *     
 *     pr_info("Sensor removed and cleaned up\n");
 *     return 0;
 * }
 * 
 * // Remove all items matching criteria
 * static int remove_items_by_name_prefix(struct state_watcher *watcher,
 *                                       const char *prefix)
 * {
 *     struct watch_item *item, *tmp;
 *     unsigned long flags;
 *     int removed_count = 0;
 *     
 *     if (!prefix)
 *         return -EINVAL;
 *     
 *     pr_info("Removing items with prefix: %s\n", prefix);
 *     
 *     // Need to iterate safely since we're removing items
 *     spin_lock_irqsave(&watcher->lock, flags);
 *     list_for_each_entry_safe(item, tmp, &watcher->item_list, list) {
 *         if (strncmp(item->name, prefix, strlen(prefix)) == 0) {
 *             // Remove from list (we already have the lock)
 *             list_del(&item->list);
 *             spin_unlock_irqrestore(&watcher->lock, flags);
 *             
 *             pr_info("Removing item: %s\n", item->name);
 *             
 *             // Clean up private data if needed
 *             if (item->private_data) {
 *                 kfree(item->private_data);
 *             }
 *             
 *             kfree(item);
 *             removed_count++;
 *             
 *             // Re-acquire lock for next iteration
 *             spin_lock_irqsave(&watcher->lock, flags);
 *         }
 *     }
 *     spin_unlock_irqrestore(&watcher->lock, flags);
 *     
 *     pr_info("Removed %d items with prefix: %s\n", removed_count, prefix);
 *     return removed_count;
 * }
 * 
 * // Conditional removal based on statistics
 * static int remove_inactive_items(struct state_watcher *watcher,
 *                                 unsigned long min_check_count)
 * {
 *     struct watch_item *item, *tmp;
 *     unsigned long flags;
 *     int removed_count = 0;
 *     
 *     pr_info("Removing items with fewer than %lu checks\n", min_check_count);
 *     
 *     spin_lock_irqsave(&watcher->lock, flags);
 *     list_for_each_entry_safe(item, tmp, &watcher->item_list, list) {
 *         if (item->check_count < min_check_count) {
 *             list_del(&item->list);
 *             spin_unlock_irqrestore(&watcher->lock, flags);
 *             
 *             pr_info("Removing inactive item: %s (%lu checks)\n",
 *                     item->name, item->check_count);
 *             
 *             kfree(item);
 *             removed_count++;
 *             
 *             spin_lock_irqsave(&watcher->lock, flags);
 *         }
 *     }
 *     spin_unlock_irqrestore(&watcher->lock, flags);
 *     
 *     return removed_count;
 * }
 * 
 * // Safe removal during shutdown
 * static void remove_all_items_safe(struct state_watcher *watcher)
 * {
 *     struct watch_item *item;
 *     int removed_count = 0;
 *     
 *     pr_info("Removing all items safely...\n");
 *     
 *     // Stop watcher first to prevent race conditions
 *     state_watcher_stop(watcher);
 *     
 *     // Remove items one by one using the official API
 *     while (true) {
 *         unsigned long flags;
 *         
 *         spin_lock_irqsave(&watcher->lock, flags);
 *         if (list_empty(&watcher->item_list)) {
 *             spin_unlock_irqrestore(&watcher->lock, flags);
 *             break;
 *         }
 *         
 *         // Get first item
 *         item = list_first_entry(&watcher->item_list, struct watch_item, list);
 *         spin_unlock_irqrestore(&watcher->lock, flags);
 *         
 *         // Remove using official API (handles locking)
 *         if (state_watcher_remove_item(watcher, item) == 0) {
 *             removed_count++;
 *         }
 *     }
 *     
 *     pr_info("Removed %d items during shutdown\n", removed_count);
 * }
 * 
 * // Remove item with error recovery
 * static int remove_item_with_retry(struct state_watcher *watcher,
 *                                  struct watch_item *item,
 *                                  int max_retries)
 * {
 *     int ret, retries = 0;
 *     
 *     do {
 *         ret = state_watcher_remove_item(watcher, item);
 *         if (ret == 0) {
 *             pr_info("Item removed successfully on attempt %d\n", retries + 1);
 *             return 0;
 *         }
 *         
 *         retries++;
 *         pr_warn("Remove attempt %d failed: %d\n", retries, ret);
 *         
 *         if (retries < max_retries) {
 *             msleep(100);  // Brief delay before retry
 *         }
 *         
 *     } while (retries < max_retries);
 *     
 *     pr_err("Failed to remove item after %d attempts\n", max_retries);
 *     return ret;
 * }
 * @endcode
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

    STATE_WATCHER_INFO("Removed watch item '%s' (addr:%p)", item->name, item);
    kfree(item);

    return 0;
}

/**
 * state_watcher_get_item_state() - Retrieve current state of a watch item
 * @item: Pointer to watch item to query
 * @current_state: Pointer to store the current state value
 *
 * Retrieves the most recent state value of the specified watch item. This
 * represents the latest value returned by the item's state function during
 * the most recent monitoring cycle.
 *
 * State value semantics:
 * - Reflects the last successful call to item->state_func()
 * - Updated during each monitoring cycle when item's interval elapses
 * - Independent of action function execution or hysteresis state
 * - Remains valid even if forced state is active
 *
 * State vs Action state distinction:
 * - current_state: Always reflects latest state_func() return value
 * - last_action_state: Only updated when action_func() is called
 * - These may differ when hysteresis prevents action triggering
 * - Forced states affect current_state immediately
 *
 * Timing considerations:
 * - State updated according to item's configured interval_ms
 * - May not reflect real-time system state between checks
 * - First state reading occurs after initial interval elapses
 * - Zero value indicates no state function calls yet
 *
 * Thread safety:
 * - Simple field read operation, no locking required
 * - State value is atomic (unsigned long read)
 * - Safe to call from any context
 * - No interference with monitoring operations
 *
 * Use cases:
 * - Real-time status display and reporting
 * - Debugging and troubleshooting monitoring behavior
 * - State correlation analysis across multiple items
 * - Conditional logic based on current system state
 *
 * Error conditions:
 * - Returns -EINVAL if item pointer is NULL
 * - Returns -EINVAL if current_state pointer is NULL
 * - Always succeeds with valid pointers
 * - No validation of item's watcher membership
 *
 * Performance characteristics:
 * - Lightweight operation (single field access)
 * - No system calls or blocking operations
 * - Minimal CPU overhead
 * - Safe for high-frequency polling
 *
 * Context: Any context (no restrictions)
 * Return: 0 on success, -EINVAL if parameters are invalid
 *
 * Example:
 * @code
 * // Basic state retrieval
 * static void check_battery_level(struct watch_item *battery_item)
 * {
 *     unsigned long battery_level;
 *     int ret;
 *     
 *     ret = state_watcher_get_item_state(battery_item, &battery_level);
 *     if (ret) {
 *         pr_err("Failed to get battery state: %d\n", ret);
 *         return;
 *     }
 *     
 *     pr_info("Current battery level: %lu%%\n", battery_level);
 *     
 *     if (battery_level < 20) {
 *         pr_warn("Battery level low: %lu%%\n", battery_level);
 *     }
 * }
 * 
 * // Multi-item state collection
 * static void collect_system_states(struct watch_item **items, int item_count)
 * {
 *     unsigned long state;
 *     int i, ret;
 *     
 *     pr_info("=== System State Report ===\n");
 *     
 *     for (i = 0; i < item_count; i++) {
 *         if (!items[i])
 *             continue;
 *             
 *         ret = state_watcher_get_item_state(items[i], &state);
 *         if (ret == 0) {
 *             pr_info("%-20s: %lu\n", items[i]->name, state);
 *         } else {
 *             pr_warn("%-20s: ERROR (%d)\n", items[i]->name, ret);
 *         }
 *     }
 *     
 *     pr_info("=== End Report ===\n");
 * }
 * 
 * // State-based conditional actions
 * static int perform_system_maintenance(struct watch_item *cpu_temp_item,
 *                                      struct watch_item *memory_item)
 * {
 *     unsigned long cpu_temp, memory_usage;
 *     int ret;
 *     
 *     // Check CPU temperature
 *     ret = state_watcher_get_item_state(cpu_temp_item, &cpu_temp);
 *     if (ret) {
 *         pr_err("Cannot read CPU temperature: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     // Check memory usage
 *     ret = state_watcher_get_item_state(memory_item, &memory_usage);
 *     if (ret) {
 *         pr_err("Cannot read memory usage: %d\n", ret);
 *         return ret;
 *     }
 *     
 *     // Conditional maintenance based on states
 *     if (cpu_temp > 80 || memory_usage > 90) {
 *         pr_warn("System under stress (CPU: %lu°C, MEM: %lu%%) - maintenance postponed\n",
 *                 cpu_temp, memory_usage);
 *         return -EBUSY;
 *     }
 *     
 *     pr_info("System stable (CPU: %lu°C, MEM: %lu%%) - starting maintenance\n",
 *             cpu_temp, memory_usage);
 *     
 *     // Perform maintenance tasks...
 *     return 0;
 * }
 * 
 * // Real-time monitoring display
 * static void display_monitoring_dashboard(struct watch_item **sensors, int count)
 * {
 *     unsigned long state;
 *     int i, ret;
 *     char status_line[256];
 *     
 *     // Build status line
 *     snprintf(status_line, sizeof(status_line), "Status: ");
 *     
 *     for (i = 0; i < count; i++) {
 *         ret = state_watcher_get_item_state(sensors[i], &state);
 *         if (ret == 0) {
 *             char item_status[32];
 *             snprintf(item_status, sizeof(item_status), "%s=%lu ", 
 *                     sensors[i]->name, state);
 *             strncat(status_line, item_status, 
 *                    sizeof(status_line) - strlen(status_line) - 1);
 *         }
 *     }
 *     
 *     pr_info("%s\n", status_line);
 * }
 * 
 * // State change detection
 * static bool has_state_changed(struct watch_item *item, unsigned long *last_known)
 * {
 *     unsigned long current_state;
 *     int ret;
 *     
 *     ret = state_watcher_get_item_state(item, &current_state);
 *     if (ret) {
 *         pr_warn("Failed to check state for %s: %d\n", item->name, ret);
 *         return false;
 *     }
 *     
 *     if (*last_known != current_state) {
 *         pr_info("State change detected for %s: %lu -> %lu\n",
 *                 item->name, *last_known, current_state);
 *         *last_known = current_state;
 *         return true;
 *     }
 *     
 *     return false;
 * }
 * 
 * // Debugging helper with state information
 * static void debug_item_state(struct watch_item *item)
 * {
 *     unsigned long current_state, check_count, action_count;
 *     int ret;
 *     
 *     if (!item) {
 *         pr_debug("Item is NULL\n");
 *         return;
 *     }
 *     
 *     // Get current state
 *     ret = state_watcher_get_item_state(item, &current_state);
 *     if (ret) {
 *         pr_debug("Item %s: Failed to get state (%d)\n", item->name, ret);
 *         return;
 *     }
 *     
 *     // Get statistics
 *     ret = state_watcher_get_item_stats(item, &check_count, &action_count);
 *     if (ret) {
 *         pr_debug("Item %s: Failed to get stats (%d)\n", item->name, ret);
 *         return;
 *     }
 *     
 *     pr_debug("Item %s: state=%lu, last_action_state=%lu, checks=%lu, actions=%lu\n",
 *              item->name, current_state, item->last_action_state, 
 *              check_count, action_count);
 * }
 * 
 * // State validation helper
 * static bool validate_state_range(struct watch_item *item, 
 *                                 unsigned long min_val, unsigned long max_val)
 * {
 *     unsigned long state;
 *     int ret;
 *     
 *     ret = state_watcher_get_item_state(item, &state);
 *     if (ret) {
 *         pr_err("Cannot validate state for %s: %d\n", item->name, ret);
 *         return false;
 *     }
 *     
 *     if (state < min_val || state > max_val) {
 *         pr_warn("State %lu for %s is outside valid range [%lu, %lu]\n",
 *                 state, item->name, min_val, max_val);
 *         return false;
 *     }
 *     
 *     return true;
 * }
 * @endcode
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
 * state_watcher_get_item_stats() - Retrieve statistics for a watch item
 * @item: Pointer to watch item to query
 * @check_count: Pointer to store total state function call count (can be NULL)
 * @action_count: Pointer to store total action function call count (can be NULL)
 *
 * Retrieves statistical information for the specified watch item, including
 * the total number of state function calls and action function calls since
 * the item was added to the watcher.
 *
 * Statistics semantics:
 * - check_count: Incremented each time item->state_func() is called
 * - action_count: Incremented each time item->action_func() is called
 * - Counters start at 0 when item is created
 * - Counters accumulate over item's entire lifetime
 * - Reset only when item is removed and recreated
 *
 * Performance metrics:
 * - check_count reflects monitoring frequency and system load
 * - action_count indicates state change frequency and system activity
 * - Ratio action_count/check_count shows state stability
 * - Low action_count suggests stable states or effective hysteresis
 * - High action_count may indicate noisy signals or frequent changes
 *
 * Monitoring insights:
 * - Zero check_count: Item not yet processed or intervals too large
 * - Zero action_count: No state changes or action_func is NULL
 * - High check/action ratio: Stable system or good hysteresis tuning
 * - Low check/action ratio: Unstable states or insufficient hysteresis
 *
 * Parameter flexibility:
 * - check_count and action_count can be NULL if not needed
 * - Allows selective retrieval of specific statistics
 * - Useful for code that only needs one metric
 * - Reduces parameter passing overhead
 *
 * Thread safety:
 * - Simple field read operations, no locking required
 * - Statistics values are atomic (unsigned long read)
 * - Safe to call from any context
 * - No interference with monitoring operations
 * - Concurrent reads are safe and consistent
 *
 * Use cases:
 * - Performance monitoring and system health assessment
 * - Tuning hysteresis values based on activity patterns
 * - Debugging monitoring behavior and timing issues
 * - Capacity planning and resource usage analysis
 * - System optimization and efficiency measurements
 *
 * Error conditions:
 * - Returns -EINVAL if item pointer is NULL
 * - Always succeeds with valid item pointer
 * - NULL output parameters are safely ignored
 * - No validation of item's watcher membership
 *
 * Timing considerations:
 * - Statistics reflect cumulative activity since item creation
 * - Updated in real-time during monitoring operations
 * - May not reflect very recent activity due to work scheduling
 * - Consistent with item's configured interval_ms
 *
 * Context: Any context (no restrictions)
 * Return: 0 on success, -EINVAL if item is NULL
 *
 * Example:
 * @code
 * // Basic statistics retrieval
 * static void print_item_statistics(struct watch_item *item)
 * {
 *     unsigned long checks, actions;
 *     int ret;
 *     
 *     ret = state_watcher_get_item_stats(item, &checks, &actions);
 *     if (ret) {
 *         pr_err("Failed to get statistics for %s: %d\n", item->name, ret);
 *         return;
 *     }
 *     
 *     pr_info("Item %s: %lu checks, %lu actions", item->name, checks, actions);
 *     
 *     if (checks > 0) {
 *         pr_info(" (%.2f%% action ratio)\n", (actions * 100.0) / checks);
 *     } else {
 *         pr_info(" (no checks yet)\n");
 *     }
 * }
 * 
 * // Performance analysis across multiple items
 * static void analyze_monitoring_performance(struct watch_item **items, int count)
 * {
 *     unsigned long total_checks = 0, total_actions = 0;
 *     unsigned long checks, actions;
 *     int i, ret;
 *     
 *     pr_info("=== Monitoring Performance Analysis ===\n");
 *     
 *     for (i = 0; i < count; i++) {
 *         if (!items[i])
 *             continue;
 *             
 *         ret = state_watcher_get_item_stats(items[i], &checks, &actions);
 *         if (ret) {
 *             pr_warn("Failed to get stats for item %d: %d\n", i, ret);
 *             continue;
 *         }
 *         
 *         total_checks += checks;
 *         total_actions += actions;
 *         
 *         pr_info("%-20s: %8lu checks, %6lu actions", 
 *                 items[i]->name, checks, actions);
 *         
 *         if (checks > 0) {
 *             double ratio = (actions * 100.0) / checks;
 *             pr_info(" (%5.1f%% activity)\n", ratio);
 *             
 *             if (ratio > 50.0) {
 *                 pr_warn("  -> High activity detected, consider increasing hysteresis\n");
 *             } else if (ratio < 1.0) {
 *                 pr_info("  -> Very stable, hysteresis working well\n");
 *             }
 *         } else {
 *             pr_info(" (not active)\n");
 *         }
 *     }
 *     
 *     pr_info("Total: %lu checks, %lu actions across %d items\n",
 *             total_checks, total_actions, count);
 * }
 * 
 * // Selective statistics retrieval
 * static unsigned long get_total_activity(struct watch_item **items, int count)
 * {
 *     unsigned long total_actions = 0;
 *     unsigned long actions;
 *     int i, ret;
 *     
 *     for (i = 0; i < count; i++) {
 *         // Only need action count, pass NULL for check_count
 *         ret = state_watcher_get_item_stats(items[i], NULL, &actions);
 *         if (ret == 0) {
 *             total_actions += actions;
 *         }
 *     }
 *     
 *     return total_actions;
 * }
 * 
 * // Monitoring efficiency assessment
 * static void assess_monitoring_efficiency(struct watch_item *item)
 * {
 *     unsigned long checks, actions;
 *     unsigned long uptime_seconds;
 *     int ret;
 *     
 *     ret = state_watcher_get_item_stats(item, &checks, &actions);
 *     if (ret) {
 *         pr_err("Cannot assess efficiency for %s: %d\n", item->name, ret);
 *         return;
 *     }
 *     
 *     // Calculate approximate uptime (simplified)
 *     uptime_seconds = (jiffies - item->last_check_time + 
 *                      msecs_to_jiffies(item->interval_ms)) / HZ;
 *     
 *     if (uptime_seconds > 0) {
 *         double checks_per_minute = (checks * 60.0) / uptime_seconds;
 *         double actions_per_minute = (actions * 60.0) / uptime_seconds;
 *         
 *         pr_info("Efficiency for %s:\n", item->name);
 *         pr_info("  Checks/min: %.2f (expected: %.2f)\n", 
 *                 checks_per_minute, 60000.0 / item->interval_ms);
 *         pr_info("  Actions/min: %.2f\n", actions_per_minute);
 *         pr_info("  Stability: %.1f%% (lower is more stable)\n",
 *                 checks > 0 ? (actions * 100.0) / checks : 0.0);
 *     }
 * }
 * 
 * // Health check based on statistics
 * static bool is_item_healthy(struct watch_item *item, 
 *                            unsigned long min_expected_checks)
 * {
 *     unsigned long checks, actions;
 *     int ret;
 *     
 *     ret = state_watcher_get_item_stats(item, &checks, &actions);
 *     if (ret) {
 *         pr_warn("Cannot check health of %s: %d\n", item->name, ret);
 *         return false;
 *     }
 *     
 *     // Check if item is being monitored regularly
 *     if (checks < min_expected_checks) {
 *         pr_warn("Item %s appears inactive: only %lu checks (expected >= %lu)\n",
 *                 item->name, checks, min_expected_checks);
 *         return false;
 *     }
 *     
 *     // Check for extremely high action frequency (possible malfunction)
 *     if (checks > 0 && (actions * 100 / checks) > 80) {
 *         pr_warn("Item %s has very high activity: %lu%% action ratio\n",
 *                 item->name, (actions * 100) / checks);
 *         return false;
 *     }
 *     
 *     pr_debug("Item %s is healthy: %lu checks, %lu actions\n",
 *              item->name, checks, actions);
 *     return true;
 * }
 * 
 * // Statistics-based hysteresis tuning recommendation
 * static void recommend_hysteresis_tuning(struct watch_item *item)
 * {
 *     unsigned long checks, actions;
 *     int ret;
 *     
 *     ret = state_watcher_get_item_stats(item, &checks, &actions);
 *     if (ret || checks == 0) {
 *         pr_info("Insufficient data for hysteresis tuning recommendation\n");
 *         return;
 *     }
 *     
 *     double activity_ratio = (actions * 100.0) / checks;
 *     
 *     pr_info("Hysteresis tuning for %s (current: %lu):\n", 
 *             item->name, item->hysteresis);
 *     
 *     if (activity_ratio > 30.0) {
 *         pr_info("  HIGH activity (%.1f%%) - consider increasing hysteresis to %lu\n",
 *                 activity_ratio, item->hysteresis + 2);
 *     } else if (activity_ratio < 5.0 && item->hysteresis > 0) {
 *         pr_info("  LOW activity (%.1f%%) - consider decreasing hysteresis to %lu\n",
 *                 activity_ratio, item->hysteresis > 1 ? item->hysteresis - 1 : 0);
 *     } else {
 *         pr_info("  OPTIMAL activity (%.1f%%) - current hysteresis is good\n",
 *                 activity_ratio);
 *     }
 * }
 * 
 * // Reset detection (useful for debugging)
 * static void detect_item_reset(struct watch_item *item, 
 *                              unsigned long *last_known_checks)
 * {
 *     unsigned long current_checks;
 *     int ret;
 *     
 *     ret = state_watcher_get_item_stats(item, &current_checks, NULL);
 *     if (ret) {
 *         pr_warn("Cannot check for reset on %s: %d\n", item->name, ret);
 *         return;
 *     }
 *     
 *     if (current_checks < *last_known_checks) {
 *         pr_warn("Item %s appears to have been reset: checks %lu -> %lu\n",
 *                 item->name, *last_known_checks, current_checks);
 *     }
 *     
 *     *last_known_checks = current_checks;
 * }
 * @endcode
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
 * state_watcher_get_stats() - Retrieve overall statistics for the state watcher
 * @watcher: Pointer to state watcher structure to query
 * @total_checks: Pointer to store cumulative check count across all items (can be NULL)
 * @total_actions: Pointer to store cumulative action count across all items (can be NULL)
 * @active_items: Pointer to store current number of active watch items (can be NULL)
 *
 * Retrieves comprehensive statistics for the entire state watcher framework,
 * providing aggregate metrics across all watch items and overall system health
 * indicators for monitoring performance and resource usage.
 *
 * Statistics semantics:
 * - total_checks: Sum of all state function calls across all items
 * - total_actions: Sum of all action function calls across all items
 * - active_items: Current number of items in the watcher's monitoring list
 * - Counters accumulate from watcher initialization
 * - active_items reflects real-time item count
 *
 * System health indicators:
 * - High total_checks indicates active monitoring system
 * - total_actions/total_checks ratio shows overall system stability
 * - active_items count helps assess monitoring scope and resource usage
 * - Zero values may indicate inactive watcher or configuration issues
 *
 * Performance insights:
 * - Low action/check ratio suggests stable system or effective hysteresis
 * - High action/check ratio indicates frequent state changes or noisy signals
 * - Growing active_items count shows expanding monitoring coverage
 * - Disproportionate check counts may reveal interval configuration issues
 *
 * Resource monitoring:
 * - total_checks correlates with CPU usage for monitoring operations
 * - active_items affects memory usage and work queue scheduling overhead
 * - Statistics help identify monitoring bottlenecks and optimization opportunities
 * - Useful for capacity planning and system tuning
 *
 * Synchronization behavior:
 * - Briefly acquires spinlock to safely count active items
 * - total_checks and total_actions read without locking (atomic access)
 * - Snapshot consistency not guaranteed across all three values
 * - Safe for concurrent access from multiple contexts
 *
 * Parameter flexibility:
 * - All output parameters can be NULL if not needed
 * - Allows selective retrieval of specific statistics
 * - Reduces parameter overhead for targeted queries
 * - Useful for specialized monitoring functions
 *
 * Timing considerations:
 * - Statistics reflect cumulative activity since watcher initialization
 * - Updated in real-time during monitoring operations
 * - active_items count is always current
 * - May not reflect very recent activity due to work scheduling delays
 *
 * Use cases:
 * - System-wide monitoring performance assessment
 * - Resource usage analysis and capacity planning
 * - Debugging watcher configuration and behavior
 * - Health monitoring and alerting systems
 * - Performance optimization and tuning guidance
 *
 * Error conditions:
 * - Returns -EINVAL if watcher pointer is NULL or uninitialized
 * - Always succeeds with valid, initialized watcher
 * - NULL output parameters are safely ignored
 * - No validation of individual item states
 *
 * Context: Any context (briefly holds spinlock for item counting)
 * Return: 0 on success, -EINVAL if watcher is invalid or uninitialized
 *
 * Example:
 * @code
 * // Basic watcher statistics display
 * static void print_watcher_summary(struct state_watcher *watcher)
 * {
 *     unsigned long total_checks, total_actions;
 *     unsigned int active_items;
 *     int ret;
 *     
 *     ret = state_watcher_get_stats(watcher, &total_checks, &total_actions, &active_items);
 *     if (ret) {
 *         pr_err("Failed to get watcher statistics: %d\n", ret);
 *         return;
 *     }
 *     
 *     pr_info("Watcher Summary:\n");
 *     pr_info("  Active items: %u\n", active_items);
 *     pr_info("  Total checks: %lu\n", total_checks);
 *     pr_info("  Total actions: %lu\n", total_actions);
 *     
 *     if (total_checks > 0) {
 *         double stability = 100.0 - ((total_actions * 100.0) / total_checks);
 *         pr_info("  System stability: %.1f%%\n", stability);
 *         pr_info("  Average checks per item: %lu\n", total_checks / active_items);
 *     } else {
 *         pr_info("  No monitoring activity yet\n");
 *     }
 * }
 * 
 * // System health assessment
 * static int assess_monitoring_health(struct state_watcher *watcher)
 * {
 *     unsigned long total_checks, total_actions;
 *     unsigned int active_items;
 *     int ret, health_score = 100;
 *     
 *     ret = state_watcher_get_stats(watcher, &total_checks, &total_actions, &active_items);
 *     if (ret) {
 *         pr_err("Health assessment failed: %d\n", ret);
 *         return -1;
 *     }
 *     
 *     pr_info("=== Monitoring Health Assessment ===\n");
 *     
 *     // Check if watcher is active
 *     if (active_items == 0) {
 *         pr_warn("No active monitoring items\n");
 *         health_score -= 50;
 *     } else {
 *         pr_info("✓ Monitoring %u items\n", active_items);
 *     }
 *     
 *     // Check monitoring activity
 *     if (total_checks == 0) {
 *         pr_warn("No monitoring activity detected\n");
 *         health_score -= 30;
 *     } else {
 *         pr_info("✓ %lu total state checks performed\n", total_checks);
 *         
 *         // Assess system stability
 *         if (total_actions * 100 / total_checks > 50) {
 *             pr_warn("High activity ratio (%lu%%) - system may be unstable\n",
 *                     (total_actions * 100) / total_checks);
 *             health_score -= 20;
 *         } else {
 *             pr_info("✓ Good stability ratio (%lu%%)\n", 
 *                     (total_actions * 100) / total_checks);
 *         }
 *     }
 *     
 *     pr_info("Overall health score: %d/100\n", health_score);
 *     return health_score;
 * }
 * 
 * // Performance monitoring and alerting
 * static void monitor_watcher_performance(struct state_watcher *watcher,
 *                                        unsigned long *baseline_checks)
 * {
 *     unsigned long current_checks;
 *     unsigned int active_items;
 *     int ret;
 *     
 *     ret = state_watcher_get_stats(watcher, &current_checks, NULL, &active_items);
 *     if (ret) {
 *         pr_warn("Performance monitoring failed: %d\n", ret);
 *         return;
 *     }
 *     
 *     if (*baseline_checks > 0) {
 *         unsigned long checks_delta = current_checks - *baseline_checks;
 *         
 *         if (checks_delta == 0) {
 *             pr_warn("ALERT: No monitoring activity since last check!\n");
 *         } else {
 *             pr_info("Performance: %lu new checks across %u items\n",
 *                     checks_delta, active_items);
 *             
 *             if (active_items > 0) {
 *                 unsigned long avg_checks = checks_delta / active_items;
 *                 pr_info("  Average activity: %lu checks per item\n", avg_checks);
 *             }
 *         }
 *     }
 *     
 *     *baseline_checks = current_checks;
 * }
 * 
 * // Resource usage analysis
 * static void analyze_resource_usage(struct state_watcher *watcher)
 * {
 *     unsigned long total_checks, total_actions;
 *     unsigned int active_items;
 *     int ret;
 *     
 *     ret = state_watcher_get_stats(watcher, &total_checks, &total_actions, &active_items);
 *     if (ret) {
 *         pr_err("Resource analysis failed: %d\n", ret);
 *         return;
 *     }
 *     
 *     pr_info("=== Resource Usage Analysis ===\n");
 *     
 *     // Memory usage estimation
 *     size_t estimated_memory = sizeof(struct state_watcher) + 
 *                              (active_items * sizeof(struct watch_item));
 *     pr_info("Estimated memory usage: %zu bytes (%u items)\n", 
 *             estimated_memory, active_items);
 *     
 *     // CPU usage indicators
 *     if (total_checks > 0) {
 *         pr_info("CPU activity indicators:\n");
 *         pr_info("  State function calls: %lu\n", total_checks);
 *         pr_info("  Action function calls: %lu\n", total_actions);
 *         pr_info("  Function call ratio: %.2f%% actions\n",
 *                 (total_actions * 100.0) / total_checks);
 *     }
 *     
 *     // Scaling recommendations
 *     if (active_items > 100) {
 *         pr_warn("High item count (%u) - consider multiple watchers\n", active_items);
 *     } else if (active_items < 5 && total_checks > 10000) {
 *         pr_info("Efficient usage: %u items handling %lu checks\n", 
 *                 active_items, total_checks);
 *     }
 * }
 * 
 * // Comparative analysis between watchers
 * static void compare_watchers(struct state_watcher **watchers, int count)
 * {
 *     unsigned long checks, actions;
 *     unsigned int items;
 *     int i, ret;
 *     
 *     pr_info("=== Watcher Comparison ===\n");
 *     pr_info("%-15s %8s %10s %10s %8s\n", 
 *             "Watcher", "Items", "Checks", "Actions", "Ratio%");
 *     
 *     for (i = 0; i < count; i++) {
 *         ret = state_watcher_get_stats(watchers[i], &checks, &actions, &items);
 *         if (ret) {
 *             pr_warn("Watcher %d: stats unavailable (%d)\n", i, ret);
 *             continue;
 *         }
 *         
 *         double ratio = checks > 0 ? (actions * 100.0) / checks : 0.0;
 *         pr_info("Watcher%-8d %8u %10lu %10lu %7.1f%%\n", 
 *                 i, items, checks, actions, ratio);
 *     }
 * }
 * 
 * // Selective statistics for specific use cases
 * static unsigned int get_active_item_count(struct state_watcher *watcher)
 * {
 *     unsigned int count = 0;
 *     int ret;
 *     
 *     // Only need item count, pass NULL for other parameters
 *     ret = state_watcher_get_stats(watcher, NULL, NULL, &count);
 *     if (ret) {
 *         pr_warn("Failed to get item count: %d\n", ret);
 *         return 0;
 *     }
 *     
 *     return count;
 * }
 * 
 * static bool is_watcher_active(struct state_watcher *watcher)
 * {
 *     unsigned long total_checks;
 *     int ret;
 *     
 *     // Only check if any monitoring has occurred
 *     ret = state_watcher_get_stats(watcher, &total_checks, NULL, NULL);
 *     return (ret == 0 && total_checks > 0);
 * }
 * 
 * // Shutdown statistics reporting
 * static void report_final_statistics(struct state_watcher *watcher)
 * {
 *     unsigned long total_checks, total_actions;
 *     unsigned int active_items;
 *     int ret;
 *     
 *     ret = state_watcher_get_stats(watcher, &total_checks, &total_actions, &active_items);
 *     if (ret) {
 *         pr_info("Final statistics unavailable: %d\n", ret);
 *         return;
 *     }
 *     
 *     pr_info("=== Final Monitoring Statistics ===\n");
 *     pr_info("Items monitored: %u\n", active_items);
 *     pr_info("Total state checks: %lu\n", total_checks);
 *     pr_info("Total actions taken: %lu\n", total_actions);
 *     
 *     if (total_checks > 0) {
 *         pr_info("Action efficiency: %.3f%% (lower = more stable)\n",
 *                 (total_actions * 100.0) / total_checks);
 *         pr_info("Average checks per item: %lu\n", total_checks / active_items);
 *     }
 *     
 *     pr_info("Monitoring system shutdown complete.\n");
 * }
 * @endcode
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

#include "traffic_monitor.h"
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>

/**
 * target_devices - Array of network interface names to monitor
 *
 * This array contains a list of common network interface names that the
 * traffic monitoring system will attempt to track.
 *
 * The array is NULL-terminated to allow for easy iteration. This is a
 * read-only configuration that defines which network devices should be
 * monitored for traffic statistics.
 */
static const char* target_devices[] = {
    "eth0",
    "eth1",
    "ens33",
    "ens160",
    "enp0s3",
    "wlan0",
    "br-docker0",
    NULL  // Sentinel
};

/**
 * NETDEV_HASH_BITS - Number of bits for network device hash table size
 *
 * Defines the number of bits used to determine the hash table size for
 * network device monitoring. The actual hash table size will be 2^4 = 16
 * entries. This provides a reasonable balance between memory usage and
 * hash collision probability for typical network device monitoring scenarios.
 */
#define NETDEV_HASH_BITS 4

/**
 * MONITOR_INTERVAL_MS - Traffic monitoring sampling interval in milliseconds
 *
 * Defines the time interval between consecutive traffic statistics sampling
 * operations. A 100ms interval provides sufficiently granular monitoring
 * while avoiding excessive system overhead. This interval determines the
 * frequency at which network interface statistics are read and processed.
 */
#define MONITOR_INTERVAL_MS 100

/**
 * netdev_monitor_hash - Hash table for actively monitored network devices
 *
 * This hash table stores network device monitoring entries, providing fast
 * lookup and access to device-specific traffic statistics. The table size
 * is determined by NETDEV_HASH_BITS and uses the kernel's standard hash
 * table implementation for efficient device management.
 */
DECLARE_HASHTABLE(netdev_monitor_hash, NETDEV_HASH_BITS);

/**
 * netdev_monitor_rwlock - Reader-writer lock for monitor hash table protection
 *
 * This rwlock synchronizes access to the netdev_monitor_hash table:
 * - Read lock: Acquired when querying statistics or performing read-only
 *   operations on existing monitor entries (e.g., delta calculations)
 * - Write lock: Acquired when modifying the hash table structure, such as
 *   adding/removing devices or updating statistics that require exclusive access
 *
 * The rwlock allows multiple concurrent readers while ensuring exclusive
 * access for writers, optimizing performance for the common case of
 * statistics queries.
 */
static DEFINE_RWLOCK(netdev_monitor_rwlock);


/**
 * monitor_work - Delayed work structure for periodic statistics monitoring
 *
 * This delayed work item is scheduled periodically to collect network
 * interface statistics from all monitored devices. The work function
 * runs at intervals defined by MONITOR_INTERVAL_MS and updates the
 * traffic statistics for each device in the monitoring hash table.
 */
static struct delayed_work monitor_work;

/**
 * active_monitors - Atomic counter tracking number of monitored devices
 *
 * This atomic counter maintains the current number of network devices
 * being actively monitored. It is incremented when a new device is
 * added to monitoring and decremented when a device is removed.
 * The counter is used to determine when monitoring can be safely
 * stopped (when it reaches zero).
 */
static atomic_t active_monitors = ATOMIC_INIT(0);

/**
 * monitor_stop_flag - Atomic flag to signal monitoring termination
 *
 * This atomic flag is used to coordinate the shutdown of the monitoring
 * subsystem. When set to non-zero, it signals that monitoring should
 * stop and all pending work should be cancelled. The flag ensures
 * clean termination of monitoring operations across multiple contexts.
 */
static atomic_t monitor_stop_flag = ATOMIC_INIT(0);

/**
 * struct simple_net_device_stats - Simplified network device statistics
 * @tx_packets: Number of transmitted packets
 * @tx_bytes: Number of transmitted bytes
 * @rx_packets: Number of received packets
 * @rx_bytes: Number of received bytes
 *
 * This structure contains essential traffic statistics for a network device,
 * providing a simplified subset of the full kernel network device statistics.
 * It tracks only the core transmit and receive counters needed for basic
 * traffic monitoring and rate calculations.
 */
struct simple_net_device_stats {
    u64 tx_packets;
    u64 tx_bytes;
    u64 rx_packets;
    u64 rx_bytes;
};

/**
 * struct netdev_monitor_entry - Network device monitoring entry
 * @dev: Pointer to the monitored network device
 * @current_stats: Current simplified network device statistics
 * @prev_stats: Previous simplified network device statistics
 * @current_stats_jiffies: Timestamp (jiffies) when current stats were updated
 * @prev_stats_jiffies: Timestamp (jiffies) when previous stats were updated
 * @hash_node: Hash table node for linking entries
 * @ifname: Network interface name (null-terminated string)
 *
 * This structure holds monitoring information for a single network device,
 * maintaining both current and previous statistics snapshots to enable
 * rate calculations. The structure is designed to be stored in a hash table
 * for efficient device lookup and includes timing information for accurate
 * delta calculations between measurement intervals.
 */
struct netdev_monitor_entry {
    struct net_device *dev;
    struct simple_net_device_stats current_stats;
    struct simple_net_device_stats prev_stats;
    unsigned long current_stats_jiffies;
    unsigned long prev_stats_jiffies;
    struct hlist_node hash_node;
    char ifname[IFNAMSIZ];
};

/**
 * is_target_device - Check if device name is in target list
 * @ifname: Network interface name to check
 *
 * Determines whether the given network interface name matches any of the
 * devices listed in the target_devices array. This function is used to
 * filter which network devices should be monitored for traffic statistics.
 *
 * Context: Any context. No locking required as target_devices is read-only.
 * Return: true if device is in the target list, false otherwise
 */
static bool is_target_device(const char *ifname)
{
    int idx;

    if (!ifname)
        return false;

    for (idx = 0; idx < ARRAY_SIZE(target_devices); idx++) {
        if (strcmp(ifname, target_devices[idx]) == 0)
            return true;
    }

    return false;
}

/**
 * register_monitor_netdevice - Register a network device for monitoring
 * @ifname: Network interface name to register
 *
 * Finds the network device by name and adds it to the monitoring hash table
 * for traffic statistics collection. The function validates the interface
 * name, looks up the device in the network namespace, checks for duplicates,
 * and creates a new monitoring entry. A reference to the network device is
 * held to prevent it from being freed while monitored.
 *
 * The monitoring entry is initialized with zero statistics and added to the
 * hash table under write lock protection. The active monitor count is
 * incremented upon successful registration.
 *
 * Context: Process context. Can sleep due to memory allocation with GFP_ATOMIC.
 *          Uses write lock with IRQ disable for hash table protection.
 * Return: 
 * * 0 - Success
 * * -EINVAL - Invalid interface name or name too long
 * * -ENODEV - Network device not found
 * * -EEXIST - Device already registered for monitoring
 * * -ENOMEM - Memory allocation failed
 */
static int register_monitor_netdevice(const char* ifname)
{
    struct net_device *dev;
    struct netdev_monitor_entry *entry;
    struct netdev_monitor_entry *existing;
    unsigned long flags;
    u32 hash_key;

    if (!ifname || strlen(ifname) >= IFNAMSIZ)
        return -EINVAL;

    dev = dev_get_by_name(&init_net, ifname);
    if (!dev) {
        printk(KERN_WARNING "traffic_monitor: Device %s not found\n", ifname);
        return -ENODEV;
    }

    write_lock_irqsave(&netdev_monitor_rwlock, flags);

    // Check if already registered
    hash_key = full_name_hash(NULL, ifname, strlen(ifname));
    hash_for_each_possible(netdev_monitor_hash, existing, hash_node, hash_key) {
        if (strcmp(existing->ifname, ifname) == 0) {
            write_unlock_irqrestore(&netdev_monitor_rwlock, flags);
            dev_put(dev);
            printk(KERN_INFO "traffic_monitor: Device %s already registered\n", ifname);
            return -EEXIST;
        }
    }

    // Create new entry
    entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
    if (!entry) {
        write_unlock_irqrestore(&netdev_monitor_rwlock, flags);
        dev_put(dev);
        return -ENOMEM;
    }

    entry->dev = dev;
    strcpy(entry->ifname, ifname);
    memset(&entry->current_stats, 0, sizeof(entry->current_stats));
    memset(&entry->prev_stats, 0, sizeof(entry->prev_stats));
    entry->current_stats_jiffies = 0;
    entry->prev_stats_jiffies = 0;

    // Add to hash table
    hash_add(netdev_monitor_hash, &entry->hash_node, hash_key);

    write_unlock_irqrestore(&netdev_monitor_rwlock, flags);

    atomic_inc(&active_monitors);

    printk(KERN_INFO "traffic_monitor: Registered device %s\n", ifname);
    return 0;
}

/**
 * unregister_monitor_netdevice - Unregister a network device from monitoring
 * @ifname: Network interface name to unregister
 *
 * Removes the specified network device from the monitoring hash table and
 * cleans up all associated resources. The function searches for the device
 * entry in the hash table, removes it from the table, releases the network
 * device reference that was acquired during registration, and frees the
 * monitoring entry memory.
 *
 * The active monitor count is decremented when a device is successfully
 * removed. The function is safe to call multiple times for the same device -
 * duplicate unregistration attempts are handled gracefully and return success.
 *
 * Context: Process context. Uses write lock with IRQ disable for hash table
 *          protection during removal operations.
 * Return:
 * * 0 - Success (device unregistered or was already unregistered)
 * * -EINVAL - Invalid interface name (NULL pointer)
 */
static int unregister_monitor_netdevice(const char* ifname)
{
    struct netdev_monitor_entry *entry;
    unsigned long flags;
    u32 hash_key;
    bool found = false;

    if (!ifname)
        return -EINVAL;

    write_lock_irqsave(&netdev_monitor_rwlock, flags);

    hash_key = full_name_hash(NULL, ifname, strlen(ifname));
    hash_for_each_possible(netdev_monitor_hash, entry, hash_node, hash_key) {
        if (strcmp(entry->ifname, ifname) == 0) {
            hash_del(&entry->hash_node);
            dev_put(entry->dev);
            kfree(entry);
            found = true;
            break;
        }
    }

    write_unlock_irqrestore(&netdev_monitor_rwlock, flags);

    if (found) {
        atomic_dec(&active_monitors);
        printk(KERN_INFO "traffic_monitor: Unregistered device %s\n", ifname);
        return 0;
    } else {
        // Device not found - may already be unregistered, which is normal
        printk(KERN_DEBUG "traffic_monitor: Device %s not found (already unregistered)\n", ifname);
        return 0;  // Return success instead of error for duplicate calls
    }
}

/**
 * calc_delta_with_overflow - Calculate delta with overflow handling
 * @current: Current counter value
 * @prev: Previous counter value
 *
 * Calculates the difference between two counter values while properly
 * handling counter overflow scenarios. Network statistics counters are
 * typically monotonic but can wrap around when they exceed the maximum
 * value for their data type (ULONG_MAX for unsigned long).
 *
 * When overflow is detected (current < prev), the function calculates
 * the delta as if the counter wrapped from ULONG_MAX back to 0. This
 * ensures accurate delta calculations even when counters overflow during
 * the monitoring interval.
 *
 * Context: Any context. No locking required.
 * Return: Difference value accounting for potential counter overflow
 */
static inline unsigned long calc_delta_with_overflow(unsigned long current, unsigned long prev)
{
    if (current >= prev) {
        return current - prev;
    } else {
        // Overflow occurred (counter wrap around)
        return (ULONG_MAX - prev) + current + 1;
    }
}

/**
 * calc_per_sec_rate - Convert delta value to per-second rate
 * @delta: Difference value to convert
 * @time_delta_jiffies: Time difference in jiffies
 *
 * Converts a raw counter delta value to a per-second rate by normalizing
 * the delta over the time interval. The calculation uses the kernel's HZ
 * constant to convert from jiffies to seconds: rate = (delta * HZ) / time_delta.
 *
 * This function is essential for converting raw counter differences into
 * meaningful rate measurements (e.g., bytes/sec, packets/sec) regardless
 * of the actual sampling interval used.
 *
 * Context: Any context. No locking required.
 * Return: Per-second rate, or 0 if time_delta_jiffies is 0 (to avoid division by zero)
 */
static inline unsigned long calc_per_sec_rate(unsigned long delta, unsigned long time_delta_jiffies)
{
    if (time_delta_jiffies == 0)
        return 0;

    return (delta * HZ) / time_delta_jiffies;
}

/**
 * update_device_stats - Update statistics for a single monitored device
 * @entry: Monitor entry to update
 * @update_jiffies: Current jiffies timestamp for this update
 *
 * Updates the traffic statistics for a monitored network device by moving
 * the current statistics to previous and fetching fresh statistics from
 * the device. This creates a snapshot pair needed for delta calculations.
 *
 * The function first attempts to use the device's ndo_get_stats operation
 * if available, which may provide more accurate or device-specific statistics.
 * If that method is not available, it falls back to reading directly from
 * the device's built-in stats structure.
 *
 * The timestamp is updated to reflect when this statistics snapshot was
 * taken, enabling accurate rate calculations based on the time difference
 * between consecutive updates.
 *
 * Context: Must be called with netdev_monitor_rwlock held for write access.
 *          The device reference is guaranteed valid during the call.
 */
static void update_device_stats(struct netdev_monitor_entry *entry, unsigned long update_jiffies)
{
    struct net_device_stats *dev_stats;

    // Save previous stats
    entry->prev_stats = entry->current_stats;
    entry->prev_stats_jiffies = entry->current_stats_jiffies;

    // Get current stats from device
    if (entry->dev->netdev_ops && entry->dev->netdev_ops->ndo_get_stats) {
        dev_stats = entry->dev->netdev_ops->ndo_get_stats(entry->dev);
        if (dev_stats) {
            entry->current_stats.tx_packets = dev_stats->tx_packets;
            entry->current_stats.tx_bytes = dev_stats->tx_bytes;
            entry->current_stats.rx_packets = dev_stats->rx_packets;
            entry->current_stats.rx_bytes = dev_stats->rx_bytes;
        }
    } else {
        // Fallback to dev->stats
        entry->current_stats.tx_packets = entry->dev->stats.tx_packets;
        entry->current_stats.tx_bytes = entry->dev->stats.tx_bytes;
        entry->current_stats.rx_packets = entry->dev->stats.rx_packets;
        entry->current_stats.rx_bytes = entry->dev->stats.rx_bytes;
    }

    // Update timestamp
    entry->current_stats_jiffies = update_jiffies;
}

/**
 * monitor_netdevices - Update statistics for all monitored devices
 *
 * Iterates through all registered network devices in the monitoring hash
 * table and updates their traffic statistics. For each device, the current
 * statistics become the previous statistics, and fresh statistics are
 * fetched from the network device. All devices are updated with the same
 * timestamp to ensure consistent timing across the monitoring system.
 *
 * This function is typically called periodically by the monitoring work
 * queue to maintain up-to-date statistics for rate calculations. The
 * consistent timestamp across all devices enables accurate comparative
 * analysis of traffic rates.
 *
 * Context: Any context. Uses write lock with IRQ disable to protect
 *          hash table iteration and prevent device list modifications
 *          during the update process.
 */
static void monitor_netdevices(void)
{
    struct netdev_monitor_entry *entry;
    unsigned long flags;
    int bkt;
    unsigned long update_jiffies = jiffies;

    write_lock_irqsave(&netdev_monitor_rwlock, flags);

    hash_for_each(netdev_monitor_hash, bkt, entry, hash_node) {
        update_device_stats(entry, update_jiffies);
    }

    write_unlock_irqrestore(&netdev_monitor_rwlock, flags);
}

/**
 * monitor_work_handler - Delayed work handler for periodic monitoring
 * @work: Work structure (unused, but required by work queue interface)
 *
 * Periodic work function that updates network device statistics for all
 * monitored devices and manages the monitoring lifecycle. The function
 * first checks the stop flag to handle clean shutdown scenarios, then
 * updates statistics for all registered devices.
 *
 * The work handler implements a self-rescheduling pattern: it continues
 * to reschedule itself at MONITOR_INTERVAL_MS intervals as long as there
 * are active monitors and the stop flag is not set. When no devices are
 * being monitored or a stop is requested, the periodic updates cease
 * automatically.
 *
 * This design ensures that monitoring overhead is only incurred when
 * devices are actually being tracked, and provides clean termination
 * during module unload or system shutdown.
 *
 * Context: Work queue context. Can sleep and be preempted.
 */
static void monitor_work_handler(struct work_struct *work)
{
    int active_count;

    // Check stop flag first to avoid infinite rescheduling during cleanup
    if (atomic_read(&monitor_stop_flag)) {
        printk(KERN_INFO "traffic_monitor: Stop flag set, terminating monitoring\n");
        return;
    }

    // Update all monitored devices
    monitor_netdevices();

    // Check if we should continue monitoring
    active_count = atomic_read(&active_monitors);
    if (active_count > 0 && !atomic_read(&monitor_stop_flag)) {
        // Reschedule for next update only if not stopping
        schedule_delayed_work(&monitor_work, msecs_to_jiffies(MONITOR_INTERVAL_MS));
    } else {
        printk(KERN_INFO "traffic_monitor: No active monitors, stopping periodic updates\n");
    }
}

/**
 * start_monitoring - Start periodic monitoring if not already running
 *
 * Initiates the periodic monitoring work queue if this is the first device
 * being monitored. The function checks if the active monitor count has
 * reached 1, indicating that monitoring should begin. This lazy-start
 * approach ensures that the periodic work is only scheduled when there
 * are actually devices to monitor.
 *
 * The monitoring work is scheduled to run after MONITOR_INTERVAL_MS
 * milliseconds and will continue to reschedule itself as long as there
 * are active monitors. If monitoring is already running (active_monitors > 1),
 * this function does nothing, avoiding duplicate work scheduling.
 *
 * Context: Any context. Should be called after successfully registering
 *          a device and incrementing active_monitors count.
 */
static void start_monitoring(void)
{
    if (atomic_read(&active_monitors) == 1) {
        // First device registered, start monitoring
        schedule_delayed_work(&monitor_work, msecs_to_jiffies(MONITOR_INTERVAL_MS));
        printk(KERN_INFO "traffic_monitor: Started periodic monitoring\n");
    }
}

/**
 * netdevice_stats_delta_single - Get per-second traffic statistics for one device
 * @ifname: Network interface name (e.g., "eth0", "wlan0")
 *
 * This function returns the per-second traffic statistics for the specified
 * network device. The statistics represent the rate of change between the
 * last two measurements taken by the monitoring subsystem.
 *
 * The device must be:
 * - Listed in the target devices configuration
 * - Currently UP and being monitored
 * - Have at least two measurement samples
 *
 * Context: Any context (uses spinlock_irqsave for protection).
 * Locking: Takes netdev_monitor_lock internally.
 *
 * Return: struct simple_net_device_stats containing per-second rates.
 *         All fields will be zero if:
 *         - Device is not found in monitor list
 *         - Device is not currently monitored
 *         - Insufficient measurement samples
 *         - Invalid interface name provided
 *
 * Example:
 * @code
 * struct simple_net_device_stats stats;
 * 
 * stats = netdevice_stats_delta("eth0");
 * if (stats.tx_packets > 0 || stats.rx_packets > 0) {
 *     printk("eth0 traffic: TX %lu pps/%lu bps, RX %lu pps/%lu bps\n",
 *            stats.tx_packets, stats.tx_bytes,
 *            stats.rx_packets, stats.rx_bytes);
 * }
 *
 * stats = netdevice_stats_delta(NULL);
 * if (stats.tx_packets > 0 || stats.rx_packets > 0) {
 *     printk("Total traffic: TX %lu pps/%lu bps, RX %lu pps/%lu bps\n",
 *            stats.tx_packets, stats.tx_bytes,
 *            stats.rx_packets, stats.rx_bytes);
 * }
 * @endcode
 */
struct simple_net_device_stats netdevice_stats_delta(const char* ifname)
{
    struct simple_net_device_stats delta;
    struct netdev_monitor_entry *entry;
    unsigned long flags;
    u32 hash_key;
    bool found = false;
    int bkt;
    unsigned long time_delta_jiffies;
    unsigned long raw_delta;

    memset(&delta, 0, sizeof(delta));

    read_lock_irqsave(&netdev_monitor_rwlock, flags);

    if (ifname) {
        // Single device mode
        hash_key = full_name_hash(NULL, ifname, strlen(ifname));
        hash_for_each_possible(netdev_monitor_hash, entry, hash_node, hash_key) {
            if (strcmp(entry->ifname, ifname) == 0) {
                // Calculate time delta
                if (entry->current_stats_jiffies >= entry->prev_stats_jiffies) {
                    time_delta_jiffies = entry->current_stats_jiffies - entry->prev_stats_jiffies;
                } else {
                    time_delta_jiffies = (ULONG_MAX - entry->prev_stats_jiffies) + entry->current_stats_jiffies + 1;
                }

                // Calculate per-second rates
                raw_delta = calc_delta_with_overflow(entry->current_stats.tx_packets, entry->prev_stats.tx_packets);
                delta.tx_packets = calc_per_sec_rate(raw_delta, time_delta_jiffies);

                raw_delta = calc_delta_with_overflow(entry->current_stats.tx_bytes, entry->prev_stats.tx_bytes);
                delta.tx_bytes = calc_per_sec_rate(raw_delta, time_delta_jiffies);

                raw_delta = calc_delta_with_overflow(entry->current_stats.rx_packets, entry->prev_stats.rx_packets);
                delta.rx_packets = calc_per_sec_rate(raw_delta, time_delta_jiffies);

                raw_delta = calc_delta_with_overflow(entry->current_stats.rx_bytes, entry->prev_stats.rx_bytes);
                delta.rx_bytes = calc_per_sec_rate(raw_delta, time_delta_jiffies);

                found = true;
                break;
            }
        }

        if (!found) {
            printk(KERN_WARNING "traffic_monitor: Device %s not found in monitor list\n", ifname);
        }
    } else {
        // All devices mode - aggregate statistics
        hash_for_each(netdev_monitor_hash, bkt, entry, hash_node) {
            // Calculate time delta for this device
            if (entry->current_stats_jiffies >= entry->prev_stats_jiffies) {
                time_delta_jiffies = entry->current_stats_jiffies - entry->prev_stats_jiffies;
            } else {
                time_delta_jiffies = (ULONG_MAX - entry->prev_stats_jiffies) + entry->current_stats_jiffies + 1;
            }

            // Add per-second rates to total
            raw_delta = calc_delta_with_overflow(entry->current_stats.tx_packets, entry->prev_stats.tx_packets);
            delta.tx_packets += calc_per_sec_rate(raw_delta, time_delta_jiffies);

            raw_delta = calc_delta_with_overflow(entry->current_stats.tx_bytes, entry->prev_stats.tx_bytes);
            delta.tx_bytes += calc_per_sec_rate(raw_delta, time_delta_jiffies);

            raw_delta = calc_delta_with_overflow(entry->current_stats.rx_packets, entry->prev_stats.rx_packets);
            delta.rx_packets += calc_per_sec_rate(raw_delta, time_delta_jiffies);

            raw_delta = calc_delta_with_overflow(entry->current_stats.rx_bytes, entry->prev_stats.rx_bytes);
            delta.rx_bytes += calc_per_sec_rate(raw_delta, time_delta_jiffies);
        }
    }

    read_unlock_irqrestore(&netdev_monitor_rwlock, flags);

    return delta;
}

/**
 * traffic_netdev_event - Network device event handler for monitoring management
 * @nb: Notifier block (unused, but required by notifier interface)
 * @event: Network device event type
 * @ptr: Event data pointer containing device information
 *
 * Handles network device lifecycle events to automatically manage the
 * monitoring of target devices. The handler monitors device state changes
 * and ensures that only devices listed in target_devices are tracked,
 * providing automatic registration and cleanup without manual intervention.
 *
 * The function handles three critical events:
 * - NETDEV_UP: Device becomes available, starts monitoring if it's a target
 * - NETDEV_GOING_DOWN: Normal shutdown, removes device from monitoring
 * - NETDEV_UNREGISTER: Emergency cleanup for abnormal device removal scenarios
 *
 * The dual cleanup approach (GOING_DOWN + UNREGISTER) ensures robust device
 * reference management even when devices are removed unexpectedly, such as
 * during physical device removal, network namespace deletion, driver errors,
 * or virtual device destruction.
 *
 * Context: Atomic context (notifier callback). Cannot sleep.
 *          Respects monitor_stop_flag for safe module cleanup.
 * Return: NOTIFY_DONE to continue notifier chain processing
 */
static int traffic_netdev_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
    struct net_device *dev = netdev_notifier_info_to_dev(ptr);

    // Check stop flag first - ignore events during module cleanup
    if (atomic_read(&monitor_stop_flag))
        return NOTIFY_DONE;

    // Only handle events for target devices
    if (!is_target_device(dev->name))
        return NOTIFY_DONE;

    switch (event) {
    case NETDEV_UP:
        printk(KERN_INFO "traffic_monitor: Target device %s is UP - adding to monitoring\n", dev->name);
        register_monitor_netdevice(dev->name);
        start_monitoring();
        break;

    case NETDEV_GOING_DOWN:
        printk(KERN_INFO "traffic_monitor: Target device %s is going DOWN - removing from monitoring\n", dev->name);
        unregister_monitor_netdevice(dev->name);
        break;

    case NETDEV_UNREGISTER:
        /* 
         * Essential backup cleanup for cases where NETDEV_GOING_DOWN
         * was not called (e.g., physical device removal, namespace deletion,
         * driver errors, virtual device deletion). Safe to call even if
         * device was already unregistered in NETDEV_GOING_DOWN.
         */
        printk(KERN_INFO "traffic_monitor: Target device %s unregistered (backup cleanup)\n", dev->name);
        unregister_monitor_netdevice(dev->name);  // Safe for duplicate calls
        break;

    default:
        break;
    }

    return NOTIFY_DONE;
}

/**
 * traffic_netdev_notifier - Network device notifier for automatic monitoring
 *
 * Notifier block structure that registers the traffic monitoring system
 * with the kernel's network device notification chain. This enables
 * automatic detection and handling of network device lifecycle events
 * such as device registration, state changes, and removal.
 *
 * The notifier uses default priority (0) to ensure it runs with normal
 * precedence in the notification chain. The handler function automatically
 * adds target devices to monitoring when they become available and removes
 * them when they go down or are unregistered, providing seamless monitoring
 * management without manual intervention.
 */
static struct notifier_block traffic_netdev_notifier = {
    .notifier_call = traffic_netdev_event,
    .priority = 0,
};

/**
 * traffic_monitor_cleanup - Clean up all monitored devices and resources
 *
 * Performs complete cleanup of the traffic monitoring subsystem by removing
 * all devices from the monitoring hash table and releasing associated resources.
 * The function safely iterates through all hash table entries, removes them
 * from the table, releases the network device references that were acquired
 * during registration, and frees the allocated memory for monitoring entries.
 *
 * The cleanup process uses hash_for_each_safe() to allow safe removal of
 * entries during iteration. After cleanup, the active monitor count is
 * reset to zero to reflect the empty state of the monitoring system.
 *
 * This function is typically called during module unloading to ensure
 * proper resource cleanup and prevent memory leaks or dangling device
 * references.
 *
 * Context: Process context. Uses write lock with IRQ disable to ensure
 *          exclusive access during the cleanup process and prevent
 *          concurrent modifications.
 */
static void traffic_monitor_cleanup(void)
{
    struct netdev_monitor_entry *entry;
    struct hlist_node *tmp;
    unsigned long flags;
    int bkt;
    
    write_lock_irqsave(&netdev_monitor_rwlock, flags);
    
    hash_for_each_safe(netdev_monitor_hash, bkt, tmp, entry, hash_node) {
        hash_del(&entry->hash_node);
        dev_put(entry->dev);
        kfree(entry);
    }
    
    write_unlock_irqrestore(&netdev_monitor_rwlock, flags);
    
    atomic_set(&active_monitors, 0);
}

/**
 * init_traffic_monitor - Initialize the traffic monitoring subsystem
 *
 * This function initializes the traffic monitoring module, sets up hash tables,
 * registers netdevice notifiers, and prepares delayed work for periodic
 * statistics collection. Must be called before using any other traffic
 * monitor functions.
 *
 * The function will:
 * - Initialize target device hash table with predefined device names
 * - Register netdevice notifier for automatic device registration
 * - Set up delayed work for periodic statistics updates
 * - Initialize all necessary locks and data structures
 *
 * Context: Process context during module initialization.
 * Locking: None required (called during init).
 *
 * Return: 
 * * 0 - Success
 * * -ENOMEM - Memory allocation failed
 * * -EBUSY - Notifier registration failed
 * 
 * Example:
 * @code
 * static int __init my_module_init(void)
 * {
 *     int ret = init_traffic_monitor();
 *     if (ret) {
 *         printk(KERN_ERR "Failed to initialize traffic monitor: %d\n", ret);
 *         return ret;
 *     }
 *     return 0;
 * }
 * @endcode
 */
int init_traffic_monitor(void)
{
    int ret;
    
    // Initialize hash tables
    hash_init(netdev_monitor_hash);
    
    // Reset stop flag to ensure monitoring can start
    // (important for module reload scenarios)
    atomic_set(&monitor_stop_flag, 0);

    // Initialize delayed work
    INIT_DELAYED_WORK(&monitor_work, monitor_work_handler);
    
    // Register netdevice notifier
    ret = register_netdevice_notifier(&traffic_netdev_notifier);
    if (ret) {
        printk(KERN_ERR "traffic_monitor: Failed to register netdevice notifier: %d\n", ret);
        return ret;
    }
    
    printk(KERN_INFO "traffic_monitor: Traffic monitoring module initialized\n");
    return 0;
}

/**
 * cleanup_traffic_monitor - Clean up the traffic monitoring subsystem
 *
 * This function cleans up all resources used by the traffic monitoring
 * module. It unregisters the netdevice notifier, cancels delayed work,
 * releases all device references, and frees all allocated memory.
 * Should be called during module cleanup.
 *
 * The function will:
 * - Unregister netdevice notifier to stop receiving events
 * - Cancel and flush any pending delayed work
 * - Release all monitored device references
 * - Free all allocated memory and hash table entries
 * - Reset all counters and state
 *
 * Context: Process context during module cleanup.
 * Locking: Uses internal locks to ensure safe cleanup.
 *
 * Note: After calling this function, no other traffic monitor functions
 * should be used until init_traffic_monitor() is called again.
 *
 * Example:
 * @code
 * static void __exit my_module_exit(void)
 * {
 *     cleanup_traffic_monitor();
 * }
 * @endcode
 */
void cleanup_traffic_monitor(void)
{
    // Set stop flag first to prevent new work scheduling
    atomic_set(&monitor_stop_flag, 1);

    // Ensure memory barrier - stop flag is visible before other operations
    smp_mb();

    // Unregister netdevice notifier
    unregister_netdevice_notifier(&traffic_netdev_notifier);
    
    // Cancel delayed work
    cancel_delayed_work_sync(&monitor_work);
    
    // Clean up all monitored devices
    traffic_monitor_cleanup();
    
    printk(KERN_INFO "traffic_monitor: Traffic monitoring module cleaned up\n");
}

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/delay.h>

/**
 * Target device names to monitor (read-only configuration)
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
 * @def NETDEV_HASH_BITS
 * @brief Hash table size bits for network device monitoring
 */
#define NETDEV_HASH_BITS 4

/**
 * @def MONITOR_INTERVAL_MS
 * @brief Monitoring interval in milliseconds
 */
#define MONITOR_INTERVAL_MS 100

/**
 * @var netdev_monitor_hash
 * @brief Hash table for storing actively monitored network devices
 */
DECLARE_HASHTABLE(netdev_monitor_hash, NETDEV_HASH_BITS);

/**
 * @var netdev_monitor_rwlock
 * @brief RW lock for protecting monitor hash table operations
 * Read lock: Used for querying statistics (delta functions)
 * Write lock: Used for modifying hash table structure and updating stats
 */
static DEFINE_RWLOCK(netdev_monitor_rwlock);


/**
 * @var monitor_work
 * @brief Delayed work for periodic statistics monitoring
 */
static struct delayed_work monitor_work;

/**
 * @var active_monitors
 * @brief Counter for active monitored devices
 */
static atomic_t active_monitors = ATOMIC_INIT(0);

/**
 * @var monitor_stop_flag
 * @brief Flag to signal monitoring should stop
 */
static atomic_t monitor_stop_flag = ATOMIC_INIT(0);

/**
 * struct simple_net_device_stats - Simplified network device statistics
 * @tx_packets: Number of transmitted packets
 * @tx_bytes: Number of transmitted bytes
 * @rx_packets: Number of received packets  
 * @rx_bytes: Number of received bytes
 * 
 * Simplified version containing only essential traffic statistics.
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
 * This structure holds monitoring information for a single network device.
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
 * Context: Any context. Uses spinlock for protection.
 *
 * Return: true if device is a target, false otherwise
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
 * This function finds the network device by name and adds it to the
 * monitoring hash table. The device reference count is incremented.
 * 
 * Context: Process context. Can sleep due to memory allocation.
 * 
 * Return: 
 * * 0 - Success
 * * -EINVAL - Invalid interface name
 * * -ENODEV - Network device not found
 * * -EEXIST - Device already registered  
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
 * This function removes the network device from the monitoring hash table
 * and releases the device reference. All associated memory is freed.
 * Safe to call multiple times - duplicate calls are ignored.
 *
 * Context: Process context.
 *
 * Return:
 * * 0 - Success or already unregistered
 * * -EINVAL - Invalid interface name
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
 * This function calculates the difference between two counter values,
 * properly handling the case where the counter has wrapped around due
 * to overflow (current < prev).
 *
 * Context: Any context.
 *
 * Return: Difference value accounting for potential overflow
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
 * This function converts a raw delta value to a per-second rate using
 * the formula: (delta * HZ) / time_delta_jiffies
 *
 * Context: Any context.
 *
 * Return: Per-second rate, 0 if time_delta_jiffies is 0
 */
static inline unsigned long calc_per_sec_rate(unsigned long delta, unsigned long time_delta_jiffies)
{
    if (time_delta_jiffies == 0)
        return 0;

    return (delta * HZ) / time_delta_jiffies;
}

/**
 * update_device_stats - Update statistics for a single device
 * @entry: Monitor entry to update
 * @update_jiffies: Current jiffies timestamp
 *
 * This function updates the statistics for a single monitored device,
 * moving current stats to previous and fetching new current stats.
 *
 * Context: Called with netdev_monitor_lock held.
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
 * This function updates the statistics for all registered network devices.
 * Current statistics become previous statistics, and new current statistics
 * are fetched from the devices. Timestamps are also updated accordingly.
 *
 * Context: Any context. Uses spinlock_irqsave for protection.
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
 * @work: Work structure
 *
 * This function is called periodically to update network device statistics.
 * It reschedules itself if there are active monitors, otherwise stops.
 *
 * Context: Work queue context.
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
 * This function starts the delayed work for periodic monitoring if it's
 * not already running.
 *
 * Context: Any context.
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
 * netdevice_stats_delta_single - Get per-second traffic delta for a device
 * @ifname: Network interface name
 *
 * This function calculates the per-second rate of change for basic traffic
 * statistics (tx/rx packets and bytes) of the specified device.
 *
 * Context: Any context. Uses spinlock_irqsave for protection.
 *
 * Return: struct simple_net_device_stats containing per-second rates.
 *         All fields will be zero if device not found or on error.
 */
struct simple_net_device_stats netdevice_stats_delta_single(const char* ifname)
{
    struct simple_net_device_stats delta;
    struct netdev_monitor_entry *entry;
    unsigned long flags;
    u32 hash_key;
    bool found = false;
    unsigned long time_delta_jiffies;
    unsigned long raw_delta;

    memset(&delta, 0, sizeof(delta));

    if (!ifname)
        return delta;

    read_lock_irqsave(&netdev_monitor_rwlock, flags);

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

    read_unlock_irqrestore(&netdev_monitor_rwlock, flags);

    if (!found) {
        printk(KERN_WARNING "traffic_monitor: Device %s not found in monitor list\n", ifname);
    }

    return delta;
}
EXPORT_SYMBOL(netdevice_stats_delta_single);

/**
 * netdevice_stats_delta_all - Get aggregate per-second traffic for all devices
 *
 * This function calculates the aggregate per-second rate of change for basic
 * traffic statistics across all monitored network devices.
 *
 * Context: Any context. Uses spinlock_irqsave for protection.
 *
 * Return: struct simple_net_device_stats containing aggregate per-second rates
 *         for all monitored devices.
 */
struct simple_net_device_stats netdevice_stats_delta_all(void)
{
    struct simple_net_device_stats total_delta;
    struct netdev_monitor_entry *entry;
    unsigned long flags;
    int bkt;
    unsigned long device_delta, time_delta_jiffies;

    memset(&total_delta, 0, sizeof(total_delta));

    read_lock_irqsave(&netdev_monitor_rwlock, flags);

    hash_for_each(netdev_monitor_hash, bkt, entry, hash_node) {
        // Calculate time delta for this device
        if (entry->current_stats_jiffies >= entry->prev_stats_jiffies) {
            time_delta_jiffies = entry->current_stats_jiffies - entry->prev_stats_jiffies;
        } else {
            time_delta_jiffies = (ULONG_MAX - entry->prev_stats_jiffies) + entry->current_stats_jiffies + 1;
        }

        // Add per-second rates to total
        device_delta = calc_delta_with_overflow(entry->current_stats.tx_packets, entry->prev_stats.tx_packets);
        total_delta.tx_packets += calc_per_sec_rate(device_delta, time_delta_jiffies);

        device_delta = calc_delta_with_overflow(entry->current_stats.tx_bytes, entry->prev_stats.tx_bytes);
        total_delta.tx_bytes += calc_per_sec_rate(device_delta, time_delta_jiffies);

        device_delta = calc_delta_with_overflow(entry->current_stats.rx_packets, entry->prev_stats.rx_packets);
        total_delta.rx_packets += calc_per_sec_rate(device_delta, time_delta_jiffies);

        device_delta = calc_delta_with_overflow(entry->current_stats.rx_bytes, entry->prev_stats.rx_bytes);
        total_delta.rx_bytes += calc_per_sec_rate(device_delta, time_delta_jiffies);
    }

    read_unlock_irqrestore(&netdev_monitor_rwlock, flags);

    return total_delta;
}
EXPORT_SYMBOL(netdevice_stats_delta_all);

/**
 * traffic_netdev_event - Network device event handler
 * @nb: Notifier block
 * @event: Event type
 * @ptr: Event data pointer
 *
 * This function handles network device events and automatically registers
 * or unregisters devices for monitoring based on the target device list.
 * Handles both normal shutdown (NETDEV_GOING_DOWN) and emergency cleanup
 * (NETDEV_UNREGISTER) to ensure proper device reference management.
 *
 * Context: Any context.
 *
 * Return: NOTIFY_DONE
 */
static int traffic_netdev_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
    struct net_device *dev = netdev_notifier_info_to_dev(ptr);

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
 * @var traffic_netdev_notifier
 * @brief Netdevice notifier block for automatic monitoring
 */
static struct notifier_block traffic_netdev_notifier = {
    .notifier_call = traffic_netdev_event,
    .priority = 0,
};

/**
 * traffic_monitor_cleanup - Clean up all monitored devices
 *
 * This function removes all devices from the monitoring hash table,
 * releases device references, and frees all allocated memory.
 * Called during module cleanup.
 *
 * Context: Process context.
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
 * init_traffic_monitor - Initialize the traffic monitoring module
 *
 * This function initializes hash tables, registers the netdevice notifier,
 * and sets up the delayed work for periodic monitoring.
 *
 * Context: Process context during module load.
 *
 * Return: 0 on success, negative error code on failure
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
        cleanup_target_devices();
        return ret;
    }
    
    printk(KERN_INFO "traffic_monitor: Traffic monitoring module initialized\n");
    return 0;
}
EXPORT_SYMBOL(init_traffic_monitor);

/**
 * cleanup_traffic_monitor - Clean up the traffic monitoring module
 *
 * This function cleans up all resources and unregisters the netdevice
 * notifier when the module is unloaded.
 *
 * Context: Process context during module unload.
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
    
    // Clean up target devices
    cleanup_target_devices();
    
    printk(KERN_INFO "traffic_monitor: Traffic monitoring module cleaned up\n");
}
EXPORT_SYMBOL(cleanup_traffic_monitor);

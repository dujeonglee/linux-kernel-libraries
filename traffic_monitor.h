#ifndef _TRAFFIC_MONITOR_H
#define _TRAFFIC_MONITOR_H

#include <linux/types.h>

/**
 * DOC: Traffic Monitor Module Overview
 *
 * The Traffic Monitor Module provides automatic network device traffic
 * statistics monitoring with real-time rate calculations. It tracks packet
 * and byte counters for registered network interfaces and converts raw
 * statistics into meaningful per-second rates with overflow protection.
 *
 * Key features:
 * - Automatic target device detection and registration
 * - Periodic statistics collection with configurable intervals
 * - Overflow-safe delta calculations for counters and timestamps
 * - Per-device and aggregate traffic rate reporting
 * - Event-driven device lifecycle management
 * - Thread-safe hash table operations with read-write locks
 * - Clean resource management and module cleanup
 * - Support for both individual device and system-wide queries
 *
 * The module automatically monitors predefined network interfaces,
 * collecting statistics at regular intervals and providing APIs to
 * retrieve current traffic rates in packets/second and bytes/second.
 * All operations are designed to handle counter overflows and provide
 * accurate rate calculations regardless of sampling intervals.
 */

/**
 * TRAFFIC_STATS_TO_MBPS - Convert bytes per second to megabits per second
 * @bytes_per_sec: Traffic rate in bytes per second
 *
 * Converts a traffic rate from bytes per second to megabits per second (Mbps).
 * The conversion multiplies by 8 to convert bytes to bits, then divides by
 * 1,000,000 to convert to megabits.
 *
 * Return: Traffic rate in megabits per second
 */
#define TRAFFIC_STATS_TO_MBPS(bytes_per_sec) ((bytes_per_sec) * 8 / 1000000ULL)

/**
 * TRAFFIC_STATS_TO_KBPS - Convert bytes per second to kilobits per second
 * @bytes_per_sec: Traffic rate in bytes per second
 *
 * Converts a traffic rate from bytes per second to kilobits per second (Kbps).
 * The conversion multiplies by 8 to convert bytes to bits, then divides by
 * 1,000 to convert to kilobits.
 *
 * Return: Traffic rate in kilobits per second
 */
#define TRAFFIC_STATS_TO_KBPS(bytes_per_sec) ((bytes_per_sec) * 8 / 1000ULL)  

/**
 * TRAFFIC_STATS_TO_MPPS - Convert packets per second to megapackets per second
 * @packets_per_sec: Traffic rate in packets per second
 *
 * Converts a traffic rate from packets per second to megapackets per second (Mpps).
 * The conversion divides by 1,000,000 to convert to megapackets.
 *
 * Return: Traffic rate in megapackets per second
 */
#define TRAFFIC_STATS_TO_MPPS(packets_per_sec) ((packets_per_sec) / 1000000ULL)

/**
 * TRAFFIC_STATS_TO_KPPS - Convert packets per second to kilopackets per second
 * @packets_per_sec: Traffic rate in packets per second
 *
 * Converts a traffic rate from packets per second to kilopackets per second (Kpps).
 * The conversion divides by 1,000 to convert to kilopackets.
 *
 * Return: Traffic rate in kilopackets per second
 */
#define TRAFFIC_STATS_TO_KPPS(packets_per_sec) ((packets_per_sec) / 1000ULL)

/**
 * @def TRAFFIC_MONITOR_INTERVAL_MS
 * @brief Default monitoring interval in milliseconds
 */
#define TRAFFIC_MONITOR_INTERVAL_MS 100

/**
 * struct simple_net_device_stats - Simplified network device statistics
 * @tx_packets: Number of transmitted packets per second
 * @tx_bytes: Number of transmitted bytes per second
 * @rx_packets: Number of received packets per second  
 * @rx_bytes: Number of received bytes per second
 * 
 * This structure contains essential traffic statistics in per-second rates.
 * All values represent the rate of change between the last two measurements.
 * 
 * Example:
 * @code
 * struct simple_net_device_stats stats = netdevice_stats_delta_single("eth0");
 * printk("eth0: %lu pps TX, %lu bps TX, %lu pps RX, %lu bps RX\n",
 *        stats.tx_packets, stats.tx_bytes, stats.rx_packets, stats.rx_bytes);
 * @endcode
 */
struct simple_net_device_stats {
    unsigned long tx_packets;    /**< Transmitted packets per second */
    unsigned long tx_bytes;      /**< Transmitted bytes per second */
    unsigned long rx_packets;    /**< Received packets per second */
    unsigned long rx_bytes;      /**< Received bytes per second */
};

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
int init_traffic_monitor(void);

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
void cleanup_traffic_monitor(void);

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
struct simple_net_device_stats netdevice_stats_delta(const char* ifname);

#endif /* _TRAFFIC_MONITOR_H */
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
 * init_traffic_monitor() - Initialize network traffic monitoring
 */
int init_traffic_monitor(void);

/**
 * cleanup_traffic_monitor() - Clean up network traffic monitoring
 */
void cleanup_traffic_monitor(void);

/**
 * netdevice_stats_delta() - Get network device statistics delta
 */
struct simple_net_device_stats netdevice_stats_delta(const char* ifname);

#endif /* _TRAFFIC_MONITOR_H */
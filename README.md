# WLBT Monitoring System

A comprehensive Linux kernel monitoring framework. This project provides robust, high-performance monitoring capabilities with state watching, traffic monitoring, and adaptive watchdog systems.

## 🚀 Features

### 📊 State Watcher Framework
- **Configurable Hysteresis**: Prevents state flapping with adjustable consecutive count thresholds
- **Flexible Intervals**: Per-item monitoring intervals with automatic validation
- **Forced State Testing**: Override states for testing and debugging scenarios
- **Comprehensive Statistics**: Real-time monitoring metrics and performance analysis
- **Thread-Safe Operations**: Spinlock-protected operations with work queue integration

### 🌐 Network Traffic Monitor
- **Real-Time Traffic Analysis**: Per-second packet and byte rate calculations
- **Automatic Device Detection**: Monitors predefined network interfaces
- **Overflow-Safe Calculations**: Handles counter wraparound scenarios
- **Event-Driven Management**: Automatic device registration and cleanup
- **Hash Table Optimization**: Fast device lookup and statistics retrieval

### ⚡ Adaptive Watchdog System
- **Lock-Free Hot Paths**: Start/cancel operations without spinlocks
- **On-Demand Scheduling**: Zero CPU overhead when inactive
- **Adaptive Period Adjustment**: Automatically optimizes checking frequency
- **Continuous Recovery**: Repeated recovery function calls until cancelled
- **Safety Limits**: Prevents system overload with minimum timeout enforcement

## 📁 Project Structure

```
wlbt-monitoring/
├── state_watcher.h          # State monitoring framework API
├── state_watcher.c          # State watcher implementation
├── traffic_monitor.h        # Network traffic monitoring API  
├── traffic_monitor.c        # Traffic monitor implementation
├── kernel_watchdog.h        # Adaptive watchdog system API
├── watchdog.c              # Watchdog implementation
└── README.md               # This file
```

## 🛠️ Quick Start

### Prerequisites
- Linux kernel development environment
- Kernel version 4.x or later
- GCC compiler with kernel headers

### Integration Example

```c
#include "state_watcher.h"
#include "traffic_monitor.h" 
#include "kernel_watchdog.h"

static struct state_watcher system_watcher;
static struct watchdog_item *device_watchdog;

// Initialize the monitoring systems
static int __init monitoring_init(void)
{
    int ret;
    
    // Initialize state watcher (1 second base interval)
    ret = state_watcher_init(&system_watcher, 1000);
    if (ret) {
        pr_err("Failed to initialize state watcher: %d\n", ret);
        return ret;
    }
    
    // Initialize traffic monitor
    ret = init_traffic_monitor();
    if (ret) {
        pr_err("Failed to initialize traffic monitor: %d\n", ret);
        goto cleanup_state_watcher;
    }
    
    // Initialize watchdog system
    ret = watchdog_init();
    if (ret) {
        pr_err("Failed to initialize watchdog: %d\n", ret);
        goto cleanup_traffic_monitor;
    }
    
    // Start monitoring
    state_watcher_start(&system_watcher);
    
    pr_info("WLBT monitoring system initialized\n");
    return 0;

cleanup_traffic_monitor:
    cleanup_traffic_monitor();
cleanup_state_watcher:
    state_watcher_cleanup(&system_watcher);
    return ret;
}

static void __exit monitoring_exit(void)
{
    // Cleanup in reverse order
    if (device_watchdog) {
        watchdog_cancel(device_watchdog);
        watchdog_remove(device_watchdog);
    }
    watchdog_deinit();
    
    state_watcher_stop(&system_watcher);
    state_watcher_cleanup(&system_watcher);
    
    cleanup_traffic_monitor();
    
    pr_info("WLBT monitoring system cleaned up\n");
}

module_init(monitoring_init);
module_exit(monitoring_exit);
```

## 📚 Usage Examples

### State Monitoring

```c
// Battery level monitoring with hysteresis
static unsigned long battery_state_func(void *private_data)
{
    return read_battery_level(); // Returns 0-100
}

static void battery_action_func(unsigned long old_state, unsigned long new_state, void *data)
{
    if (new_state <= 20 && old_state > 20) {
        pr_warn("Battery low: %lu%%\n", new_state);
        trigger_low_battery_actions();
    }
}

// Add battery monitor with 5-second interval and 2-count hysteresis
struct watch_item_init battery_init = {
    .name = "battery_monitor",
    .interval_ms = 5000,
    .hysteresis = 2,
    .state_func = battery_state_func,
    .action_func = battery_action_func
};

struct watch_item *battery_item = state_watcher_add_item(&watcher, &battery_init);

// Force low battery state for testing (10% for 30 seconds)
state_watcher_force_state(battery_item, 10, 30000);
```

### Traffic Monitoring

```c
// Get traffic statistics for specific interface
struct simple_net_device_stats stats = netdevice_stats_delta("eth0");
if (stats.rx_bytes > 0 || stats.tx_bytes > 0) {
    pr_info("eth0: RX %lu Mbps, TX %lu Mbps\n",
            TRAFFIC_STATS_TO_MBPS(stats.rx_bytes),
            TRAFFIC_STATS_TO_MBPS(stats.tx_bytes));
}

// Get aggregate statistics for all monitored interfaces  
struct simple_net_device_stats total = netdevice_stats_delta(NULL);
pr_info("Total traffic: %lu pps RX, %lu pps TX\n", 
        total.rx_packets, total.tx_packets);
```

### Watchdog Monitoring

```c
// Recovery function called on timeout
static void device_recovery(void *data)
{
    struct my_device *dev = (struct my_device *)data;
    pr_warn("Device timeout - attempting reset\n");
    my_device_reset(dev);
}

// Create 5-second timeout watchdog
struct watchdog_item *wdog = watchdog_add(5000, device_recovery, my_dev);

// Start monitoring before critical operation
watchdog_start(wdog);
int result = perform_critical_operation();
if (result == 0) {
    watchdog_cancel(wdog); // Success - cancel timeout
}
// If operation fails/hangs, recovery function will be called
```

## 🔧 Configuration

### State Watcher Configuration

```c
#define DEFAULT_STATE_WATCHER_INTERVAL_MS 200  // Base interval
#define DEFAULT_HYSTERESIS 0                   // No hysteresis by default

// All item intervals must be multiples of base_interval_ms
// Example: base=200ms, valid intervals: 200ms, 400ms, 600ms, 1000ms, etc.
```

### Traffic Monitor Configuration

```c
#define MONITOR_INTERVAL_MS 100  // Statistics sampling interval

// Target devices automatically monitored
static const char* target_devices[] = {
    "eth0", "eth1", "ens33", "ens160", "enp0s3", 
    "wlan0", "br-docker0", NULL
};
```

### Watchdog Configuration

```c
#define WATCHDOG_MIN_TIMEOUT_MS 200        // Minimum timeout (safety limit)
#define WATCHDOG_MAX_WORK_PERIOD_MS 100    // Maximum work frequency
```

## 📈 Performance Characteristics

### State Watcher
- **Memory Usage**: ~150 bytes per watch item + watcher overhead
- **CPU Overhead**: Configurable via base interval (200ms default)
- **Scalability**: Handles hundreds of items efficiently
- **Latency**: Sub-millisecond state change detection

### Traffic Monitor  
- **Memory Usage**: ~200 bytes per monitored interface
- **CPU Overhead**: 100ms sampling with hash table optimization
- **Accuracy**: Counter overflow protection, precise rate calculations
- **Interfaces**: Supports unlimited network interfaces

### Watchdog System
- **Memory Usage**: ~100 bytes per watchdog item
- **CPU Overhead**: Adaptive (zero when idle, optimized when active)
- **Accuracy**: Adaptive period adjustment (min_timeout/2)
- **Performance**: Lock-free start/cancel operations

## 🐛 Debugging & Testing

### Debug Features
- Comprehensive logging with `pr_debug`, `pr_info`, `pr_warn`, `pr_err`
- Statistics collection for performance analysis
- Forced state testing for validation scenarios

### Testing Tools
```c
// Force specific states for testing
state_watcher_force_state(item, test_value, duration_ms);

// Monitor statistics
unsigned long checks, actions;
state_watcher_get_item_stats(item, &checks, &actions);

// Validate system health
unsigned long total_checks, total_actions;
unsigned int active_items;
state_watcher_get_stats(&watcher, &total_checks, &total_actions, &active_items);
```

## ⚠️ Safety & Limitations

### Safety Features
- **Minimum timeout enforcement**: Prevents system overload (200ms minimum)
- **Overflow protection**: Safe counter arithmetic for traffic statistics  
- **Memory safety**: Atomic validity flags prevent use-after-free
- **Resource cleanup**: Automatic cleanup on module unload

### Limitations
- **Kernel space only**: Not suitable for userspace applications
- **Single instance**: One global context per monitoring system
- **Timing accuracy**: Limited by kernel timer resolution (jiffies)

## 📝 License

This project is licensed under the GPL-2.0 License - see individual source files for details.

## 🤝 Contributing

1. Follow Linux kernel coding standards
2. Include comprehensive documentation for new features
3. Add test cases for new functionality
4. Ensure thread safety for all operations
5. Maintain backward compatibility

## 📞 Support

For issues related to:
- **State Watcher**: Check interval validation and hysteresis configuration
- **Traffic Monitor**: Verify target device list and network interface status
- **Watchdog System**: Ensure minimum timeout requirements are met

## 🔄 Version History

- **v1.0.0**: Initial release with complete monitoring framework
  - State watcher with hysteresis support
  - Network traffic monitoring with overflow protection
  - Adaptive watchdog system with lock-free operations
  - Comprehensive documentation and examples
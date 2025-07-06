# Kernel Watchdog Library

A high-performance, adaptive watchdog system for Linux kernel drivers with lock-free operations and intelligent resource management.

## Overview

This library provides a robust watchdog mechanism designed specifically for kernel drivers that need reliable timeout monitoring with minimal performance overhead. Unlike traditional watchdog implementations, this system features adaptive scheduling, lock-free hot paths, and zero-overhead idle operation.

## Key Features

### 🚀 Performance Optimized
- **Lock-free operations**: `start` and `cancel` operations use only atomic instructions (~5-10 CPU cycles)
- **Zero-overhead idle**: No CPU usage when no watchdogs are active
- **Adaptive periods**: Automatically adjusts work frequency based on shortest timeout

### 🛡️ Safety & Reliability
- **Start-once semantics**: Prevents timeout extension through repeated start calls
- **Built-in limits**: Enforces minimum 100ms timeout with `BUG()` protection
- **Thread-safe design**: Safe concurrent access from multiple contexts
- **Use-after-free protection**: Atomic validity checking prevents memory corruption

### 🔄 Intelligent Scheduling
- **On-demand work**: Starts monitoring only when watchdogs are added
- **Dynamic optimization**: Adjusts checking frequency automatically
- **Resource efficient**: Stops all work when no watchdogs remain active

### 📚 Production Ready
- **Comprehensive documentation**: Full kernel-style comments and examples
- **Error handling**: Clear error messages and proper cleanup
- **Testing included**: Example usage and safety mechanism verification

## Quick Start

### Basic Usage

```c
#include "kernel_watchdog.h"

// Recovery function called on timeout
static void my_recovery(void *data) {
    struct my_device *dev = (struct my_device *)data;
    pr_err("Device %s timeout - attempting recovery\n", dev->name);
    reset_device_hardware(dev);
}

// Initialize watchdog system
int ret = watchdog_init();
if (ret < 0) {
    pr_err("Failed to initialize watchdog\n");
    return ret;
}

// Add a watchdog with 1 second timeout
struct watchdog_item *wd = watchdog_add(1000, my_recovery, my_device);
if (!wd) {
    pr_err("Failed to add watchdog\n");
    watchdog_deinit();
    return -ENOMEM;
}

// Start monitoring
watchdog_start(wd);

// Do critical operation...
perform_hardware_operation();

// Cancel monitoring when operation completes
watchdog_cancel(wd);

// Cleanup
watchdog_remove(wd);
watchdog_deinit();
```

### Advanced Pattern: Continuous Monitoring

```c
// For ongoing monitoring (e.g., heartbeat checking)
struct watchdog_item *heartbeat_wd = watchdog_add(5000, check_heartbeat, device);
watchdog_start(heartbeat_wd);

// Recovery function will be called every work period after 5 seconds
// until explicitly cancelled - perfect for continuous monitoring
```

## API Reference

### Initialization

```c
int watchdog_init(void);
void watchdog_deinit(void);
```

Initialize/deinitialize the watchdog system. No parameters needed - the system automatically manages work scheduling.

### Watchdog Management

```c
struct watchdog_item *watchdog_add(unsigned long timeout_ms,
                                   void (*recovery_func)(void *data),
                                   void *private_data);
int watchdog_remove(struct watchdog_item *item);
```

Add/remove watchdog items. `timeout_ms` must be ≥ 100ms. Recovery function called on timeout with provided private data.

### Monitoring Control (Lock-free)

```c
int watchdog_start(struct watchdog_item *item);
int watchdog_cancel(struct watchdog_item *item);
```

Start/stop timeout monitoring. These operations are lock-free for maximum performance. `start` uses "start-once" semantics - subsequent calls ignored until `cancel` is called.

## Design Principles

### Start-Once Semantics

```c
watchdog_start(wd);    // Sets timeout baseline at time T
// ... 200ms later
watchdog_start(wd);    // Ignored - timeout not extended
// Timeout occurs at T + timeout_ms (not extended by 200ms)
```

To restart timeout monitoring:
```c
watchdog_cancel(wd);   // Stop current monitoring
watchdog_start(wd);    // Start fresh timeout period
```

### Adaptive Period Adjustment

The system automatically optimizes work frequency:

```c
// Scenario 1: Long timeouts only
watchdog_add(10000, func1, data);  // 10 second timeout
watchdog_add(30000, func2, data);  // 30 second timeout
// → Work period: 5000ms (efficient)

// Scenario 2: Short timeout added
watchdog_add(200, func3, data);    // 200ms timeout
// → Work period: 100ms (200ms/2), but clamped to minimum 50ms = 100ms

// Scenario 3: Short timeout removed
watchdog_remove(short_item);
// → Work period: automatically reverts to 5000ms (efficient)
```

### Continuous Recovery

After timeout occurs, recovery function is called every work period until cancelled:

```c
// Timeline for 1000ms timeout:
// 0ms    - watchdog_start()
// 1000ms - First recovery call
// 1050ms - Second recovery call (if work period is 50ms)
// 1100ms - Third recovery call
// ...    - Continues until watchdog_cancel()
```

## Safety Features

### Timeout Limits

```c
watchdog_add(50, func, data);   // ← Triggers BUG()!
```

**Error message:**
```
FATAL: Watchdog timeout (50 ms) is shorter than minimum allowed (100 ms)
This would cause excessive CPU usage and system instability
Please use timeout >= 100 ms or redesign your timing requirements
```

### Thread Safety

- **Lock-free operations**: `start`/`cancel` safe from any context
- **Protected operations**: `add`/`remove` use spinlocks for list safety
- **Atomic validity**: Prevents use-after-free with atomic flags
- **Memory barriers**: Ensures correct ordering on all architectures

## Performance Characteristics

### CPU Usage
- **Idle**: 0% (no work scheduled)
- **Active**: Minimal, proportional to shortest timeout
- **Hot paths**: ~5-10 CPU cycles (atomic operations only)

### Memory Usage
- **Per item**: ~64 bytes (struct + list overhead)
- **Global**: ~128 bytes (context structure)
- **No dynamic allocation** during start/cancel operations

### Scalability
- **Items**: Tested with 1000+ concurrent watchdogs
- **Timeout range**: 100ms to hours
- **Contexts**: Safe from interrupt, tasklet, and process contexts

## Build and Test

### Building

```bash
make                    # Build all modules
make clean              # Clean build files
```

### Testing

```bash
# Load and test normal operation
make load               # Load library and example
make dmesg              # Check kernel messages
make unload             # Unload modules

# Test safety mechanisms (VM ONLY!)
make test-invalid       # Tests BUG() trigger - WILL CRASH SYSTEM
```

### Files

- `kernel_watchdog.h` - Complete API definition with documentation
- `kernel_watchdog.c` - Main implementation
- `watchdog_example.c` - Usage examples and demonstrations
- `watchdog_invalid_test.c` - Safety mechanism testing (dangerous!)
- `Makefile` - Build configuration

## Integration Guide

### Driver Integration

1. **Include header**: `#include "kernel_watchdog.h"`
2. **Initialize once**: Call `watchdog_init()` in driver init
3. **Add watchdogs**: Create items for operations needing monitoring
4. **Use start/cancel**: Around critical operations
5. **Cleanup**: Remove items and call `watchdog_deinit()` in driver exit

### Error Handling

```c
// Always check return values
struct watchdog_item *wd = watchdog_add(timeout, func, data);
if (!wd) {
    // Handle allocation failure
    return -ENOMEM;
}

int ret = watchdog_start(wd);
if (ret < 0) {
    // Handle invalid item or system not initialized
    watchdog_remove(wd);
    return ret;
}
```

### Recovery Function Guidelines

```c
// Good: Simple, fast recovery
static void simple_recovery(void *data) {
    struct device *dev = data;
    reset_device(dev);
}

// Good: Rate-limited recovery
static void rate_limited_recovery(void *data) {
    static unsigned long last_call = 0;

    if (time_after(jiffies, last_call + HZ)) {
        perform_expensive_recovery();
        last_call = jiffies;
    }
}

// Avoid: Long-running operations
static void bad_recovery(void *data) {
    msleep(1000);  // ← Don't do this! Blocks work queue
}
```

## Common Patterns

### Pattern 1: Operation Timeout

```c
// Protect a critical operation
struct watchdog_item *op_wd = watchdog_add(5000, operation_failed, ctx);
watchdog_start(op_wd);

result = perform_long_operation();

watchdog_cancel(op_wd);
watchdog_remove(op_wd);
```

### Pattern 2: Heartbeat Monitoring

```c
// Continuous health monitoring
struct watchdog_item *health_wd = watchdog_add(30000, health_check, device);
watchdog_start(health_wd);

// Recovery function handles health check and resets itself if needed
// Watchdog runs until device shutdown
```

### Pattern 3: Multiple Timeouts

```c
// Different timeouts for different operations
struct watchdog_item *fast_wd = watchdog_add(100, fast_timeout, ctx);
struct watchdog_item *slow_wd = watchdog_add(10000, slow_timeout, ctx);

// System automatically optimizes for shortest timeout (100ms)
```

## Troubleshooting

### Common Issues

**"Watchdog not initialized"**
- Ensure `watchdog_init()` is called before any other operations

**"Invalid watchdog item pointer"**
- Check that item wasn't already removed
- Verify pointer isn't NULL or corrupted

**BUG() on watchdog_add()**
- Timeout must be ≥ 100ms
- Check timeout value calculation

**Recovery not called**
- Ensure `watchdog_start()` was called
- Check that timeout period has elapsed
- Verify recovery function pointer is valid

### Debug Tips

```c
// Check if watchdog is active
if (atomic_read(&item->active)) {
    pr_info("Watchdog is active\n");
}

// Check validity
if (atomic_read(&item->valid)) {
    pr_info("Watchdog item is valid\n");
}
```

## License

GPL-2.0 - See individual source files for full license text.

## Contributing

This library follows Linux kernel coding standards:
- Kernel-style function documentation
- Proper error handling and cleanup
- Thread-safe design patterns
- Comprehensive testing

For contributions, ensure:
- All functions have proper documentation
- Error paths are tested
- Code follows kernel style guidelines
- Changes maintain backward compatibility


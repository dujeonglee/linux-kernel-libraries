#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include "monitor.h"

/* 예제 전역 변수 */
static struct monitor_manager g_monitor_mgr;
static struct proc_dir_entry *proc_entry;

/* 모니터 아이템 포인터들 */
static struct monitor_item *cpu_item;
static struct monitor_item *memory_item;
static struct monitor_item *temp_item;

/* 예제 private data 구조체 */
struct example_data {
    char name[32];
    unsigned long threshold;
    unsigned long counter;
};

/* 예제 1: CPU 사용률 시뮬레이션 모니터 함수 */
static unsigned long cpu_usage_monitor(void *private_data)
{
    struct example_data *data = (struct example_data *)private_data;
    unsigned long usage;
    
    /* 실제로는 CPU 사용률을 읽어야 하지만, 여기서는 랜덤 값으로 시뮬레이션 */
    get_random_bytes(&usage, sizeof(usage));
    usage = usage % 100;  /* 0-99% 사용률 */
    
    data->counter++;
    
    monitor_debug("CPU usage monitor: %lu%% (counter: %lu)", usage, data->counter);
    return usage;
}

/* 예제 1: CPU 사용률 액션 함수 */
static void cpu_usage_action(unsigned long old_state, unsigned long new_state, void *private_data)
{
    struct example_data *data = (struct example_data *)private_data;
    
    if (new_state > data->threshold) {
        monitor_info("CPU usage alert: %s - Usage increased from %lu%% to %lu%% (threshold: %lu%%)",
                     data->name, old_state, new_state, data->threshold);
    } else {
        monitor_info("CPU usage normal: %s - Usage decreased from %lu%% to %lu%%",
                     data->name, old_state, new_state);
    }
}

/* 예제 2: 메모리 사용량 시뮬레이션 모니터 함수 */
static unsigned long memory_usage_monitor(void *private_data)
{
    struct example_data *data = (struct example_data *)private_data;
    unsigned long usage;
    
    /* 메모리 사용량 시뮬레이션 (0-1024MB) */
    get_random_bytes(&usage, sizeof(usage));
    usage = usage % 1024;
    
    data->counter++;
    
    monitor_debug("Memory usage monitor: %lu MB (counter: %lu)", usage, data->counter);
    return usage;
}

/* 예제 2: 메모리 사용량 액션 함수 */
static void memory_usage_action(unsigned long old_state, unsigned long new_state, void *private_data)
{
    struct example_data *data = (struct example_data *)private_data;
    
    if (new_state > data->threshold) {
        monitor_info("Memory usage alert: %s - Usage increased from %lu MB to %lu MB (threshold: %lu MB)",
                     data->name, old_state, new_state, data->threshold);
    } else {
        monitor_info("Memory usage normal: %s - Usage decreased from %lu MB to %lu MB",
                     data->name, old_state, new_state);
    }
}

/* 예제 3: 온도 시뮬레이션 모니터 함수 */
static unsigned long temperature_monitor(void *private_data)
{
    struct example_data *data = (struct example_data *)private_data;
    unsigned long temp;
    
    /* 온도 시뮬레이션 (20-80도) */
    get_random_bytes(&temp, sizeof(temp));
    temp = 20 + (temp % 60);
    
    data->counter++;
    
    monitor_debug("Temperature monitor: %lu°C (counter: %lu)", temp, data->counter);
    return temp;
}

/* 예제 3: 온도 액션 함수 */
static void temperature_action(unsigned long old_state, unsigned long new_state, void *private_data)
{
    struct example_data *data = (struct example_data *)private_data;
    
    if (new_state > data->threshold) {
        monitor_info("Temperature alert: %s - Temperature increased from %lu°C to %lu°C (threshold: %lu°C)",
                     data->name, old_state, new_state, data->threshold);
    } else {
        monitor_info("Temperature normal: %s - Temperature decreased from %lu°C to %lu°C",
                     data->name, old_state, new_state);
    }
}

/* proc 파일 읽기 함수 */
static int monitor_proc_show(struct seq_file *m, void *v)
{
    unsigned long total_checks, total_actions;
    unsigned int active_items;
    int ret;
    
    seq_printf(m, "Monitor Library Example Status\n");
    seq_printf(m, "==============================\n\n");
    
    ret = monitor_get_manager_stats(&g_monitor_mgr, &total_checks, &total_actions, &active_items);
    if (ret == 0) {
        seq_printf(m, "Manager Status:\n");
        seq_printf(m, "  Running: %s\n", g_monitor_mgr.running ? "Yes" : "No");
        seq_printf(m, "  Base Interval: %lu ms\n", g_monitor_mgr.base_interval_ms);
        seq_printf(m, "  Active Items: %u\n", active_items);
        seq_printf(m, "  Total Checks: %lu\n", total_checks);
        seq_printf(m, "  Total Actions: %lu\n", total_actions);
        seq_printf(m, "\n");
    }
    
    /* 개별 아이템 상태 표시 */
    seq_printf(m, "Individual Item Status:\n");
    
    if (cpu_item && !IS_ERR(cpu_item)) {
        unsigned long state, checks, actions;
        monitor_get_item_state(cpu_item, &state);
        monitor_get_item_stats(cpu_item, &checks, &actions);
        seq_printf(m, "  CPU Monitor (%p):\n", cpu_item);
        seq_printf(m, "    Name: %s\n", cpu_item->name);
        seq_printf(m, "    Current State: %lu\n", state);
        seq_printf(m, "    Checks: %lu, Actions: %lu\n", checks, actions);
        seq_printf(m, "    Interval: %lu ms, Hysteresis: %lu\n", 
                   cpu_item->interval_ms, cpu_item->hysteresis);
        seq_printf(m, "\n");
    }
    
    if (memory_item && !IS_ERR(memory_item)) {
        unsigned long state, checks, actions;
        monitor_get_item_state(memory_item, &state);
        monitor_get_item_stats(memory_item, &checks, &actions);
        seq_printf(m, "  Memory Monitor (%p):\n", memory_item);
        seq_printf(m, "    Name: %s\n", memory_item->name);
        seq_printf(m, "    Current State: %lu\n", state);
        seq_printf(m, "    Checks: %lu, Actions: %lu\n", checks, actions);
        seq_printf(m, "    Interval: %lu ms, Hysteresis: %lu\n", 
                   memory_item->interval_ms, memory_item->hysteresis);
        seq_printf(m, "\n");
    }
    
    if (temp_item && !IS_ERR(temp_item)) {
        unsigned long state, checks, actions;
        monitor_get_item_state(temp_item, &state);
        monitor_get_item_stats(temp_item, &checks, &actions);
        seq_printf(m, "  Temperature Monitor (%p):\n", temp_item);
        seq_printf(m, "    Name: %s\n", temp_item->name);
        seq_printf(m, "    Current State: %lu\n", state);
        seq_printf(m, "    Checks: %lu, Actions: %lu\n", checks, actions);
        seq_printf(m, "    Interval: %lu ms, Hysteresis: %lu\n", 
                   temp_item->interval_ms, temp_item->hysteresis);
        seq_printf(m, "\n");
    }
    
    seq_printf(m, "Note: Use 'dmesg | grep monitor' to see detailed monitor logs\n");
    
    return 0;
}

static int monitor_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, monitor_proc_show, NULL);
}

static const struct proc_ops monitor_proc_ops = {
    .proc_open = monitor_proc_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

/* 모듈 초기화 */
static int __init monitor_example_init(void)
{
    int ret;
    
    /* 예제 데이터 할당 */
    static struct example_data cpu_data = {
        .name = "CPU",
        .threshold = 80,  /* 80% 임계값 */
        .counter = 0
    };
    
    static struct example_data memory_data = {
        .name = "Memory",
        .threshold = 512,  /* 512MB 임계값 */
        .counter = 0
    };
    
    static struct example_data temp_data = {
        .name = "Temperature",
        .threshold = 70,  /* 70도 임계값 */
        .counter = 0
    };
    
    printk(KERN_INFO "Monitor example module loading...\n");
    
    /* 모니터 매니저 초기화 */
    ret = monitor_manager_init(&g_monitor_mgr, 2000);  /* 2초 기본 주기 */
    if (ret) {
        printk(KERN_ERR "Failed to initialize monitor manager: %d\n", ret);
        return ret;
    }
    
    /* CPU 모니터 아이템 추가 */
    struct monitor_item_init cpu_init = {
        .name = "cpu_usage",
        .interval_ms = 2000,  /* 2초 주기 (base_interval 2000의 1배) */
        .hysteresis = 3,      /* 3번 연속 동일값 필요 */
        .monitor_func = cpu_usage_monitor,
        .action_func = cpu_usage_action,
        .private_data = &cpu_data
    };
    
    cpu_item = monitor_add_item(&g_monitor_mgr, &cpu_init);
    if (IS_ERR(cpu_item)) {
        ret = PTR_ERR(cpu_item);
        printk(KERN_ERR "Failed to add CPU monitor item: %d\n", ret);
        goto cleanup_mgr;
    }
    printk(KERN_INFO "Added CPU monitor item at %p\n", cpu_item);
    
    /* 메모리 모니터 아이템 추가 */
    struct monitor_item_init memory_init = {
        .name = "memory_usage",
        .interval_ms = 4000,  /* 4초 주기 (base_interval 2000의 2배) */
        .hysteresis = 2,      /* 2번 연속 동일값 필요 */
        .monitor_func = memory_usage_monitor,
        .action_func = memory_usage_action,
        .private_data = &memory_data
    };
    
    memory_item = monitor_add_item(&g_monitor_mgr, &memory_init);
    if (IS_ERR(memory_item)) {
        ret = PTR_ERR(memory_item);
        printk(KERN_ERR "Failed to add memory monitor item: %d\n", ret);
        goto cleanup_mgr;
    }
    printk(KERN_INFO "Added memory monitor item at %p\n", memory_item);
    
    /* 온도 모니터 아이템 추가 */
    struct monitor_item_init temp_init = {
        .name = "temperature",
        .interval_ms = 6000,  /* 6초 주기 (base_interval 2000의 3배) */
        .hysteresis = 4,      /* 4번 연속 동일값 필요 */
        .monitor_func = temperature_monitor,
        .action_func = temperature_action,
        .private_data = &temp_data
    };
    
    temp_item = monitor_add_item(&g_monitor_mgr, &temp_init);
    if (IS_ERR(temp_item)) {
        ret = PTR_ERR(temp_item);
        printk(KERN_ERR "Failed to add temperature monitor item: %d\n", ret);
        goto cleanup_mgr;
    }
    printk(KERN_INFO "Added temperature monitor item at %p\n", temp_item); memory_usage_action,
        .private_data = &memory_data
    };
    
    ret = monitor_add_item(&g_monitor_mgr, &memory_init, &item_id);
    if (ret) {
        printk(KERN_ERR "Failed to add memory monitor item: %d\n", ret);
        goto cleanup_mgr;
    }
    printk(KERN_INFO "Added memory monitor item with ID: %u\n", item_id);
    
    /* 온도 모니터 아이템 추가 */
    struct monitor_item_init temp_init = {
        .name = "temperature",
        .interval_ms = 5000,  /* 5초 주기 */
        .hysteresis = 4,      /* 4번 연속 동일값 필요 */
        .monitor_func = temperature_monitor,
        .action_func = temperature_action,
        .private_data = &temp_data
    };
    
    ret = monitor_add_item(&g_monitor_mgr, &temp_init, &item_id);
    if (ret) {
        printk(KERN_ERR "Failed to add temperature monitor item: %d\n", ret);
        goto cleanup_mgr;
    }
    printk(KERN_INFO "Added temperature monitor item with ID: %u\n", item_id);
    
    /* proc 파일 시스템 엔트리 생성 */
    proc_entry = proc_create("monitor_example", 0444, NULL, &monitor_proc_ops);
    if (!proc_entry) {
        printk(KERN_ERR "Failed to create proc entry\n");
        ret = -ENOMEM;
        goto cleanup_mgr;
    }
    
    /* 모니터 시작 */
    ret = monitor_start(&g_monitor_mgr);
    if (ret) {
        printk(KERN_ERR "Failed to start monitor: %d\n", ret);
        goto cleanup_proc;
    }
    
    printk(KERN_INFO "Monitor example module loaded successfully\n");
    printk(KERN_INFO "Check /proc/monitor_example for status\n");
    printk(KERN_INFO "Use 'dmesg | grep monitor' to see monitor logs\n");
    
    return 0;
    
cleanup_proc:
    proc_remove(proc_entry);
cleanup_mgr:
    monitor_manager_cleanup(&g_monitor_mgr);
    return ret;
}

/* 모듈 정리 */
static void __exit monitor_example_exit(void)
{
    printk(KERN_INFO "Monitor example module unloading...\n");
    
    /* 모니터 정지 */
    monitor_stop(&g_monitor_mgr);
    
    /* proc 파일 시스템 엔트리 제거 */
    if (proc_entry) {
        proc_remove(proc_entry);
    }
    
    /* 개별 모니터 아이템 제거 */
    if (cpu_item && !IS_ERR(cpu_item)) {
        monitor_remove_item(&g_monitor_mgr, cpu_item);
    }
    if (memory_item && !IS_ERR(memory_item)) {
        monitor_remove_item(&g_monitor_mgr, memory_item);
    }
    if (temp_item && !IS_ERR(temp_item)) {
        monitor_remove_item(&g_monitor_mgr, temp_item);
    }
    
    /* 모니터 매니저 정리 */
    monitor_manager_cleanup(&g_monitor_mgr);
    
    printk(KERN_INFO "Monitor example module unloaded successfully\n");
}

module_init(monitor_example_init);
module_exit(monitor_example_exit);

MODULE_AUTHOR("Monitor Library Example");
MODULE_DESCRIPTION("Example usage of Linux Kernel Monitor Library");
MODULE_VERSION("1.0.0");
MODULE_LICENSE("GPL");
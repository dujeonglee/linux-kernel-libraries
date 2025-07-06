#include "monitor.h"

/**
 * monitor_state_changed_with_hysteresis - Check if monitor state has changed with hysteresis
 * @item: monitor item to check
 * @new_state: new state value returned by monitor function
 *
 * This function implements hysteresis based on consecutive count mechanism.
 * State change is recognized only when the new state value appears 
 * consecutively for the specified hysteresis count.
 *
 * Hysteresis behavior:
 * - hysteresis = 0: immediate state change recognition
 * - hysteresis = N: new state must appear N times consecutively
 *
 * The function maintains internal state:
 * - candidate_state: the potential new state being evaluated
 * - consecutive_count: number of consecutive occurrences of candidate_state
 *
 * Context: Called from monitor work function with manager lock held
 * Return: true if state change should trigger action, false otherwise
 */
static bool monitor_state_changed_with_hysteresis(struct monitor_item *item, 
                                                  unsigned long new_state)
{
    /* 히스테리시스가 0이면 즉시 변화로 인정 */
    if (item->hysteresis == 0) {
        return item->last_action_state != new_state;
    }
    
    /* 현재 상태와 동일하면 변화 없음 */
    if (item->last_action_state == new_state) {
        item->consecutive_count = 0;
        item->candidate_state = new_state;
        return false;
    }
    
    /* 후보 상태와 동일한지 확인 */
    if (item->candidate_state == new_state) {
        item->consecutive_count++;
        monitor_debug("Item %s: consecutive count %lu for state %lu (need %lu)",
                     item->name, item->consecutive_count, new_state, item->hysteresis);
        
        /* 히스테리시스 조건 만족 시 상태 변화로 인정 */
        if (item->consecutive_count >= item->hysteresis) {
            item->consecutive_count = 0;
            return true;
        }
    } else {
        /* 새로운 후보 상태 */
        item->candidate_state = new_state;
        item->consecutive_count = 1;
        monitor_debug("Item %s: new candidate state %lu (count 1, need %lu)",
                     item->name, new_state, item->hysteresis);
    }
    
    return false;
}

/* 모니터 워크 함수 */
static void monitor_work_func(struct work_struct *work)
{
    struct monitor_manager *mgr = container_of(work, struct monitor_manager, work.work);
    struct monitor_item *item, *tmp;
    unsigned long current_time = jiffies;
    unsigned long flags;
    
    if (!mgr->running) {
        return;
    }
    
    spin_lock_irqsave(&mgr->lock, flags);
    
    /* 모든 모니터 아이템 순회 */
    list_for_each_entry_safe(item, tmp, &mgr->item_list, list) {
        /* 아이템의 주기가 되었는지 확인 */
        if (time_after(current_time, item->last_check_time + 
                      msecs_to_jiffies(item->interval_ms))) {
            
            unsigned long new_state;
            
            /* 모니터 함수 호출 */
            if (item->monitor_func) {
                new_state = item->monitor_func(item->private_data);
                item->check_count++;
                mgr->total_checks++;
                
                monitor_debug("Item %s: state %lu -> %lu", 
                             item->name, item->current_state, new_state);
                
                /* 히스테리시스를 적용하여 상태 변화 확인 */
                if (monitor_state_changed_with_hysteresis(item, new_state)) {
                    /* 상태가 변경되었으므로 액션 함수 호출 */
                    if (item->action_func) {
                        /* 스핀락 해제 후 액션 함수 호출 (액션 함수에서 슬립 가능) */
                        spin_unlock_irqrestore(&mgr->lock, flags);
                        
                        monitor_debug("Item %s: executing action, state change %lu -> %lu",
                                     item->name, item->last_action_state, new_state);
                        
                        item->action_func(item->last_action_state, new_state, item->private_data);
                        
                        spin_lock_irqsave(&mgr->lock, flags);
                        
                        /* 리스트가 변경되었을 수 있으므로 다시 확인 */
                        if (!mgr->running) {
                            spin_unlock_irqrestore(&mgr->lock, flags);
                            return;
                        }
                        
                        item->last_action_state = new_state;
                        item->action_count++;
                        mgr->total_actions++;
                    }
                }
                
                item->current_state = new_state;
                item->last_check_time = current_time;
            }
        }
    }
    
    spin_unlock_irqrestore(&mgr->lock, flags);
    
    /* 다음 주기에 다시 실행되도록 스케줄 */
    if (mgr->running) {
        schedule_delayed_work(&mgr->work, msecs_to_jiffies(mgr->base_interval_ms));
    }
}

/* 모니터 매니저 초기화 */
int monitor_manager_init(struct monitor_manager *mgr, unsigned long base_interval_ms)
{
    if (!mgr) {
        return -EINVAL;
    }
    
    memset(mgr, 0, sizeof(*mgr));
    
    INIT_LIST_HEAD(&mgr->item_list);
    INIT_DELAYED_WORK(&mgr->work, monitor_work_func);
    spin_lock_init(&mgr->lock);
    
    mgr->base_interval_ms = base_interval_ms ? base_interval_ms : DEFAULT_MONITOR_INTERVAL_MS;
    mgr->running = false;
    mgr->initialized = true;
    
    monitor_info("Monitor manager initialized with base interval %lu ms", 
                 mgr->base_interval_ms);
    
    return 0;
}

/* 모니터 매니저 정리 */
void monitor_manager_cleanup(struct monitor_manager *mgr)
{
    struct monitor_item *item, *tmp;
    unsigned long flags;
    
    if (!mgr || !mgr->initialized) {
        return;
    }
    
    /* 모니터 정지 */
    monitor_stop(mgr);
    
    /* 모든 아이템 제거 */
    spin_lock_irqsave(&mgr->lock, flags);
    list_for_each_entry_safe(item, tmp, &mgr->item_list, list) {
        list_del(&item->list);
        kfree(item);
    }
    spin_unlock_irqrestore(&mgr->lock, flags);
    
    mgr->initialized = false;
    
    monitor_info("Monitor manager cleaned up");
}

/* 모니터 시작 */
int monitor_start(struct monitor_manager *mgr)
{
    if (!mgr || !mgr->initialized) {
        return -EINVAL;
    }
    
    if (mgr->running) {
        return -EALREADY;
    }
    
    mgr->running = true;
    schedule_delayed_work(&mgr->work, msecs_to_jiffies(mgr->base_interval_ms));
    
    monitor_info("Monitor started");
    return 0;
}

/* 모니터 정지 */
void monitor_stop(struct monitor_manager *mgr)
{
    if (!mgr || !mgr->initialized) {
        return;
    }
    
    mgr->running = false;
    cancel_delayed_work_sync(&mgr->work);
    
    monitor_info("Monitor stopped");
}

/* 모니터 아이템 추가 */
struct monitor_item *monitor_add_item(struct monitor_manager *mgr, 
                                     const struct monitor_item_init *init)
{
    struct monitor_item *item;
    unsigned long flags;
    unsigned long interval_ms;
    
    if (!mgr || !mgr->initialized || !init || !init->monitor_func) {
        return ERR_PTR(-EINVAL);
    }
    
    /* interval_ms 유효성 검사 */
    interval_ms = init->interval_ms ? init->interval_ms : mgr->base_interval_ms;
    
    /* interval_ms가 base_interval_ms의 배수인지 확인 */
    if (interval_ms % mgr->base_interval_ms != 0) {
        monitor_err("Invalid interval %lu ms: must be multiple of base interval %lu ms",
                   interval_ms, mgr->base_interval_ms);
        return ERR_PTR(-EINVAL);
    }
    
    /* interval_ms가 base_interval_ms보다 작으면 안됨 */
    if (interval_ms < mgr->base_interval_ms) {
        monitor_err("Invalid interval %lu ms: must be >= base interval %lu ms",
                   interval_ms, mgr->base_interval_ms);
        return ERR_PTR(-EINVAL);
    }
    
    item = kzalloc(sizeof(*item), GFP_KERNEL);
    if (!item) {
        return ERR_PTR(-ENOMEM);
    }
    
    /* 아이템 초기화 */
    INIT_LIST_HEAD(&item->list);
    item->interval_ms = interval_ms;
    item->hysteresis = init->hysteresis;
    item->monitor_func = init->monitor_func;
    item->action_func = init->action_func;
    item->private_data = init->private_data;
    
    /* 초기 상태 설정 */
    item->current_state = 0;
    item->last_action_state = 0;
    item->last_check_time = jiffies;
    
    /* 히스테리시스 상태 초기화 */
    item->candidate_state = 0;
    item->consecutive_count = 0;
    
    /* 통계 초기화 */
    item->check_count = 0;
    item->action_count = 0;
    
    /* 이름 설정 */
    if (init->name) {
        strncpy(item->name, init->name, sizeof(item->name) - 1);
        item->name[sizeof(item->name) - 1] = '\0';
    } else {
        snprintf(item->name, sizeof(item->name), "item_%p", item);
    }
    
    spin_lock_irqsave(&mgr->lock, flags);
    
    /* 리스트에 추가 */
    list_add_tail(&item->list, &mgr->item_list);
    
    spin_unlock_irqrestore(&mgr->lock, flags);
    
    monitor_info("Added monitor item '%s' (addr:%p, interval:%lu ms, hysteresis:%lu)",
                 item->name, item, item->interval_ms, item->hysteresis);
    
    return item;
}

/* 모니터 아이템 제거 */
int monitor_remove_item(struct monitor_manager *mgr, struct monitor_item *item)
{
    unsigned long flags;
    
    if (!mgr || !mgr->initialized || !item) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&mgr->lock, flags);
    
    /* 리스트에서 제거 */
    list_del(&item->list);
    
    spin_unlock_irqrestore(&mgr->lock, flags);
    
    monitor_info("Removed monitor item '%s' (addr:%p)", item->name, item);
    kfree(item);
    
    return 0;
}

/* 모니터 아이템 상태 조회 */
int monitor_get_item_state(struct monitor_item *item, unsigned long *current_state)
{
    if (!item || !current_state) {
        return -EINVAL;
    }
    
    *current_state = item->current_state;
    return 0;
}

/* 모니터 아이템 통계 조회 */
int monitor_get_item_stats(struct monitor_item *item,
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

/* 모니터 매니저 통계 조회 */
int monitor_get_manager_stats(struct monitor_manager *mgr,
                             unsigned long *total_checks, unsigned long *total_actions,
                             unsigned int *active_items)
{
    struct monitor_item *item;
    unsigned long flags;
    unsigned int count = 0;
    
    if (!mgr || !mgr->initialized) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&mgr->lock, flags);
    
    if (total_checks) {
        *total_checks = mgr->total_checks;
    }
    if (total_actions) {
        *total_actions = mgr->total_actions;
    }
    
    if (active_items) {
        list_for_each_entry(item, &mgr->item_list, list) {
            count++;
        }
        *active_items = count;
    }
    
    spin_unlock_irqrestore(&mgr->lock, flags);
    return 0;
}

/* 모듈 정보 */
MODULE_AUTHOR("Monitor Library");
MODULE_DESCRIPTION("Linux Kernel Monitor Library");
MODULE_VERSION(MONITOR_VERSION);
MODULE_LICENSE("GPL");
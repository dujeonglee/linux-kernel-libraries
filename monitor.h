#ifndef _MONITOR_H
#define _MONITOR_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/jiffies.h>

/* 모니터 라이브러리 버전 */
#define MONITOR_VERSION "1.0.0"

/* 기본 설정값 */
#define DEFAULT_MONITOR_INTERVAL_MS 1000  /* 기본 모니터링 주기 1초 */
#define DEFAULT_HYSTERESIS 0              /* 기본 히스테리시스 값 */

/* 모니터 함수 타입 정의 */
typedef unsigned long (*monitor_func_t)(void *private_data);
typedef void (*action_func_t)(unsigned long old_state, unsigned long new_state, void *private_data);

/* 모니터 아이템 구조체 */
struct monitor_item {
    struct list_head list;              /* 리스트 연결을 위한 노드 */
    
    /* 사용자 설정 파라미터 */
    unsigned long interval_ms;          /* 모니터링 주기 (밀리초) */
    unsigned long hysteresis;           /* 히스테리시스 값 (연속 횟수) */
    monitor_func_t monitor_func;        /* 모니터 함수 포인터 */
    action_func_t action_func;          /* 액션 함수 포인터 */
    void *private_data;                 /* 사용자 private 데이터 */
    
    /* 내부 상태 관리 */
    unsigned long current_state;        /* 현재 상태값 */
    unsigned long last_action_state;    /* 마지막으로 액션이 실행된 상태값 */
    unsigned long last_check_time;      /* 마지막 체크 시간 */
    
    /* 히스테리시스 관리 */
    unsigned long candidate_state;      /* 후보 상태값 */
    unsigned long consecutive_count;    /* 연속된 동일 값 횟수 */
    
    /* 식별자 */
    char name[32];                      /* 아이템 이름 (사용자 지정) */
    
    /* 통계 정보 */
    unsigned long check_count;          /* 체크 횟수 */
    unsigned long action_count;         /* 액션 실행 횟수 */
};

/* 모니터 관리자 구조체 */
struct monitor_manager {
    struct list_head item_list;         /* 모니터 아이템 리스트 */
    struct delayed_work work;           /* 주기적 작업을 위한 워크큐 */
    spinlock_t lock;                    /* 동기화를 위한 스핀락 */
    
    /* 관리자 설정 */
    unsigned long base_interval_ms;     /* 기본 체크 주기 */
    
    /* 상태 플래그 */
    bool running;                       /* 실행 중 여부 */
    bool initialized;                   /* 초기화 완료 여부 */
    
    /* 통계 정보 */
    unsigned long total_checks;         /* 전체 체크 횟수 */
    unsigned long total_actions;        /* 전체 액션 실행 횟수 */
};

/* 모니터 아이템 초기화 구조체 */
struct monitor_item_init {
    const char *name;                   /* 아이템 이름 */
    unsigned long interval_ms;          /* 모니터링 주기 */
    unsigned long hysteresis;           /* 히스테리시스 값 (연속 횟수) */
    monitor_func_t monitor_func;        /* 모니터 함수 */
    action_func_t action_func;          /* 액션 함수 */
    void *private_data;                 /* private 데이터 */
};

/* 함수 선언 */

/* 모니터 매니저 초기화 및 정리 */
int monitor_manager_init(struct monitor_manager *mgr, unsigned long base_interval_ms);
void monitor_manager_cleanup(struct monitor_manager *mgr);

/* 모니터 시작 및 정지 */
int monitor_start(struct monitor_manager *mgr);
void monitor_stop(struct monitor_manager *mgr);

/* 모니터 아이템 관리 */
struct monitor_item *monitor_add_item(struct monitor_manager *mgr, 
                                     const struct monitor_item_init *init);
int monitor_remove_item(struct monitor_manager *mgr, struct monitor_item *item);

/* 모니터 아이템 상태 조회 */
int monitor_get_item_state(struct monitor_item *item, unsigned long *current_state);
int monitor_get_item_stats(struct monitor_item *item,
                          unsigned long *check_count, unsigned long *action_count);

/* 모니터 매니저 상태 조회 */
int monitor_get_manager_stats(struct monitor_manager *mgr,
                             unsigned long *total_checks, unsigned long *total_actions,
                             unsigned int *active_items);

/* 유틸리티 함수 */
static inline bool monitor_state_changed_with_hysteresis(struct monitor_item *item, 
                                                        unsigned long new_state)
{
    /* 히스테리시스가 0이면 즉시 변화로 인정 */
    if (item->hysteresis == 0) {
        return item->last_action_state != new_state;
    }
    
    /* 현재 상태와 동일하면 변화 없음 */
    if (item->last_action_state == new_state) {
        item->consecutive_count = 0;
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

/* 디버그 매크로 */
#ifdef DEBUG
#define monitor_debug(fmt, ...) \
    printk(KERN_DEBUG "monitor: " fmt "\n", ##__VA_ARGS__)
#else
#define monitor_debug(fmt, ...) do { } while (0)
#endif

#define monitor_info(fmt, ...) \
    printk(KERN_INFO "monitor: " fmt "\n", ##__VA_ARGS__)

#define monitor_warn(fmt, ...) \
    printk(KERN_WARNING "monitor: " fmt "\n", ##__VA_ARGS__)

#define monitor_err(fmt, ...) \
    printk(KERN_ERR "monitor: " fmt "\n", ##__VA_ARGS__)

#endif /* _MONITOR_H */
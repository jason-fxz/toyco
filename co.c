#include "co.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>

static inline void
stack_switch_call(void *sp, void *entry, uintptr_t arg) {
    asm volatile (
#if __x86_64__
        "movq %0, %%rsp; movq %2, %%rdi; jmp *%1"
          :
          : "b"((uintptr_t)sp - 8),
            "d"(entry),
            "a"(arg)
          : "memory"
#else
        "movl %0, %%esp; movl %2, 4(%0); jmp *%1"
        :
        : "b"((uintptr_t)sp - 8),
            "d"(entry),
            "a"(arg)
            : "memory"
            #endif
    );
}

// #define DEBUG

#ifdef DEBUG
#define debug(fmt, ...) fprintf(stderr, "\033[90m[debug] " fmt "\033[0m", ##__VA_ARGS__)
#else
#define debug(fmt, ...)
#endif

#define panic(fmt, ...) do { \
    fprintf(stderr, "\033[31mPANIC\033[0m at %s:%d in %s: " fmt, __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    exit(1); \
} while (0)

#define CO_STACK_SIZE (32 * 1024) // 32KB
#define MAX_CO_NUM 1024

void co_wrapper(struct co *co);


enum co_status {
    CO_NEW = 1, // 新创建，还未执行过
    CO_RUNNING, // 已经执行过
    CO_WAITING, // 在 co_wait 上等待
    CO_DEAD,    // 已经结束，但还未释放资源
};

struct co {
    char *name;
    void (*func)(void *); // co_start 指定的入口地址和参数
    void *arg;
    
    enum co_status status;  // 协程的状态
    struct co *    waiter;  // 是否有其他协程在等待当前协程
    jmp_buf        context; // 寄存器现场
    uint8_t        stack_begin[0]; // 用于获取 stack 的起始
    uint8_t        stack[CO_STACK_SIZE]; // 协程的堆栈
    uint8_t        stack_end[0]; // 用于获取 stack 的末尾
};

struct co_table {
    struct co *tab[MAX_CO_NUM];
    int num;
};

struct co_table co_wait_table;
struct co_table co_run_table;
struct co_table co_dead_table;

void co_table_init(struct co_table *table) {
    table->num = 0;
    for (int i = 0; i < MAX_CO_NUM; i++) {
        table->tab[i] = NULL;
    }
}

void co_table_add(struct co_table *table, struct co *co) {
    table->tab[table->num++] = co;
}

void co_table_del_idx(struct co_table *table, int index) {
    // move the tail to the deleted position
    table->tab[index] = table->tab[--table->num];
    table->tab[table->num] = NULL;
}

void co_table_del_co(struct co_table *table, struct co *co) {
    for (int i = 0; i < table->num; i++) {
        if (table->tab[i] == co) {
            co_table_del_idx(table, i);
            return ;
        }
    }
}

struct co* co_current;
#define current (co_current)

static struct co main_co;

__attribute__((constructor))
static void co_init() {
    debug("co_init\n");
    // 初始化协程表
    co_table_init(&co_wait_table);
    co_table_init(&co_run_table);
    co_table_init(&co_dead_table);
    
    // 初始化主协程
    main_co.name = "main"; 
    main_co.func = NULL;
    main_co.arg = NULL;
    main_co.status = CO_RUNNING;
    main_co.waiter = NULL;
    
    // 设置当前协程为主协程
    co_current = &main_co;
    
    // 将主协程加入运行表
    co_table_add(&co_run_table, &main_co);
    debug("co_init done! %d\n", co_run_table.num);
}


void co_schedule() {
    // random select a co from co_run_table
    int idx = rand() % co_run_table.num;
    current = co_run_table.tab[idx];
    assert(current != NULL);
    debug("co_schedule: %s\n", current->name);
    if (current->status == CO_NEW) {
        debug("co_schedule: %s -> start\n", current->name);
        stack_switch_call(current->stack + CO_STACK_SIZE, co_wrapper, (uintptr_t)current);
    } else if (current->status == CO_RUNNING) {
        debug("co_schedule: %s -> resume\n", current->name);
        longjmp(current->context, 1);
    } else {
        panic("co status is %d\n", current->status);
    }
    panic("should never reach here");
    // longjmp(current->context, 1);
}

void co_wrapper(struct co *co) {
    co->status = CO_RUNNING;
    debug("co_wrapper: %s\n", co->name);
    co->func(co->arg);
    // panic("co_wrapper: debug\n");

    current->status = CO_DEAD;
    co_table_del_co(&co_run_table, current);
    co_table_add(&co_dead_table, current);
    if (current->waiter) {
        current->waiter->status = CO_RUNNING;
        co_table_del_co(&co_wait_table, current->waiter);
        co_table_add(&co_run_table, current->waiter);
    }
    co_schedule();
}


struct co *co_start(const char *name, void (*func)(void *), void *arg) {
    struct co *co = (struct co *)malloc(sizeof(struct co));
    debug("co_start: %s\n", name);
    if (co == NULL) {
        panic("co_start: malloc failed\n");
        return NULL;
    }
    
    co->name = (char *)name;
    co->func = func;
    co->arg = arg;
    co->status = CO_NEW;
    co->waiter = NULL;
    
    co_table_add(&co_run_table, co);

    memset(co->stack, 0x5f, CO_STACK_SIZE); // for debuging
    debug("co_start: %s, stack: %p\n", name, co->stack);
    return co;
}

void co_wait(struct co *co) {
    debug("co_wait: %s (%s)\n", co->name, current->name);
    if (co->status == CO_DEAD) {
        debug("co_wait: %s (%s) -> return\n", co->name, current->name);
        return ;
    }
    current->status = CO_WAITING;
    co->waiter = current;
    
    co_table_del_co(&co_run_table, current);
    co_table_add(&co_wait_table, current);
    debug("co_wait: %s (%s) -> yield\n", co->name, current->name);
    co_yield();
}

void co_yield() {
    debug("co_yield: %s\n", current->name);
    int val = setjmp(current->context);
    if (val == 0) { // 保存当前上下文
        co_schedule();
    } else { // 恢复上下文
        return ;
    }
}

#undef current
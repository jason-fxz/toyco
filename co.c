#include "list.h"
#include "co.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>

#include "panic.h"

// this function is used to switch stack and start a function on the new stack
//   ! this function never return
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


#define CO_RUNTIME_STACK_SIZE (4 * 1024) // 4KB
#define CO_STACK_SIZE (32 * 1024) // 32KB
#define MAX_CO_NUM 1024

void co_wrapper(struct co *co);


enum co_status {
    CO_NEW = 1, // 新创建，还未执行过
    CO_RUNNING, // 已经执行过
    CO_WAITING, // 在 co_wait 上等待
    CO_DEAD,    // 已经结束，但还未释放资源
};

struct co_list_node {
    struct list_head node;
    struct co *co;
};

struct co {
    char *name;
    void (*func)(void *); // co_start 指定的入口地址和参数
    void *arg;
    
    enum co_status status;  // 协程的状态
    struct list_head waiters; // 当前协程在等待哪些协程
    jmp_buf        context; // 寄存器现场
    uint8_t        *stack;  // 协程的堆栈
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
uint8_t co_runtime_stack[CO_RUNTIME_STACK_SIZE]; // 用于 runtime 的栈

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
    INIT_LIST_HEAD(&main_co.waiters);
    main_co.stack = NULL; // 主协程不需要堆栈(直接使用系统堆栈)
    
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

void co_dead_handle(struct co *co) {
    struct co_list_node *entry, *tmp;
    co->status = CO_DEAD;
    free(co->stack); // 释放堆栈
    co->stack = NULL;
    co_table_del_co(&co_run_table, co);
    co_table_add(&co_dead_table, co);

    list_for_each_entry_safe(entry, tmp, &co->waiters, node) {
        co_table_del_co(&co_wait_table, entry->co);
        co_table_add(&co_run_table, entry->co);
        list_del(&entry->node);
        entry->co->status = CO_RUNNING;
        free(entry);
    }
    co_schedule();
}

void co_wrapper(struct co *co) {
    co->status = CO_RUNNING;
    debug("co_wrapper: %s\n", co->name);
    co->func(co->arg);
    stack_switch_call(co_runtime_stack + CO_RUNTIME_STACK_SIZE, co_dead_handle, (uintptr_t)co);
}


struct co *co_start(const char *name, void (*func)(void *), void *arg) {
    struct co *co = (struct co *)malloc(sizeof(struct co));
    debug("co_start: %s\n", name);
    if (co == NULL) {
        panic("malloc co_struct failed\n");
        return NULL;
    }
    
    co->name = (char *)malloc(strlen(name) + 1);
    if (co->name == NULL) {
        panic("malloc co->name failed\n");
        return NULL;
    }
    strcpy(co->name, name);
    co->func = func;
    co->arg = arg;
    co->status = CO_NEW;
    INIT_LIST_HEAD(&co->waiters);
    co->stack = (uint8_t *)malloc(CO_STACK_SIZE);
    if (co->stack == NULL) {
        panic("malloc stack failed\n");
        return NULL;
    }
    
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
    struct co_list_node *node = (struct co_list_node *)malloc(sizeof(struct co_list_node));
    if (node == NULL) {
        panic("malloc co_list_node failed\n");
        return ;
    }
    node->co = current;
    list_add(&node->node, &co->waiters);

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

void co_free(struct co *co) {
    if (!co || co == &main_co) return;
    if (co->name) {
        free(co->name);
        co->name = NULL;
    }
    if (co->stack) {
        free(co->stack);
        co->stack = NULL;
    }
    free(co);
}

__attribute__((destructor))
static void co_main_exit() {
    debug("co main exit\n");
    for (int i = 0; i < co_run_table.num; i++) {
        co_free(co_run_table.tab[i]);
    }
    for (int i = 0; i < co_wait_table.num; i++) {
        co_free(co_wait_table.tab[i]);
    }
    for (int i = 0; i < co_dead_table.num; i++) {
        co_free(co_dead_table.tab[i]);
    }
}


#undef current
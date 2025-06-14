/*
 * some internal definitions for the Coroutine library
 * this file is not part of the public API
 */

#ifndef CO_INTER_H
#define CO_INTER_H

#include "list.h"
#include <setjmp.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

enum co_status {
    CO_NEW = 1, // 新创建，还未执行过
    CO_RUNNING, // 正在运行
    CO_RUNABLE, // 可运行状态，等待调度
    CO_WAITING, // 在 co_wait 上等待
    CO_DEAD,    // 已经结束，但还未释放资源
    CO_SYSCALL  // TODO 阻塞在系统调用，此时分配了一个 M
};

// 协程结构体 co (G)
struct co {
    char *name;
    void (*func)(void *); // co_start 指定的入口地址和参数
    void *arg;
    
    // 并发安全字段 - 需要原子操作或锁保护
    atomic_int status;           // 协程的状态 (使用原子操作)

    // waiters 
    struct list_head waiters;    // 等待该协程的协程列表
    pthread_mutex_t waiters_lock; // 等待队列锁
    atomic_int waitq_size;       // 等待队列大小
    
    struct list_head node;       // 用于插入队列的节点

    // 协程上下文
    jmp_buf        context; // 寄存器现场
    uint8_t        *stack;  // 协程的栈
    size_t         stack_size; // 栈大小

    // G-P-M 模型相关字段
    // struct M *m; // 协程当前所属的 M
    struct P *p; // 协程当前所属的 P
    size_t coid; // 协程的唯一 ID (coID)
};

#define co_get_status(g)    atomic_load(&(g)->status)
#define co_set_status(g, s) atomic_store(&(g)->status, (s))

// M (Machine) - 系统线程，负责执行 G
struct M {
    int id;                    // M 的唯一 ID
    pthread_t thread;          // 对应的系统线程
    struct P *p;               // 当前绑定的 P
    struct co *cur_g;          // 当前正在运行的协程 G
    
    // 调度相关
    jmp_buf sched_context;     // 调度器上下文
    
    // 状态标志
    atomic_bool spinning;      // 是否在自旋等待工作
    atomic_bool blocked;       // 是否在系统调用中阻塞
    
    // 链表节点
    struct list_head node;     // 用于插入各种 M 队列
};

// P 的状态 (暂且无用)
enum p_status {
    P_IDLE = 0,     // 空闲状态
    P_RUNNING,      // 运行状态
    P_DEAD          // 死亡状态
};

// P (Processor) - 逻辑处理器，维护本地协程队列
struct P {
    int id;                    // P 的唯一 ID
    struct M *m;               // 当前绑定的 M
    
    // 本地协程队列
    struct list_head run_queue; // 本地运行队列，存放可运行的协程 G
    pthread_mutex_t queue_lock;  // 队列锁
    atomic_int runq_size;        // 运行队列大小
    
    // 状态管理
    enum p_status status;        // P 的状态
    struct list_head idle_node;  // 用于插入空闲 P 列表  

    // 调度信息
    uint64_t sched_tick; // 调度次数计数器
};



// 全局调度器
struct scheduler {
    // P 数组 (P 数量固定)
    struct P *all_p;           // 所有 P 的数组
    int nproc;                 // P 的数量
    
    // M 队列管理 (M 数量动态变化)
    struct list_head all_m_list;      // 所有 M 的队列
    pthread_mutex_t m_lock;           // M 队列锁
    atomic_int num_m;                 // 当前 M 总数
    atomic_int m_id_gen;              // M ID 生成器
    
    // 全局协程队列
    struct list_head global_runq;     // 全局运行队列
    pthread_mutex_t global_runq_lock; // 全局队列锁
    int global_runq_size;      // 全局运行队列大小
    
    // 死亡协程表
    struct list_head dead_co_list;     // 死亡协程列表
    pthread_mutex_t dead_co_lock;      // 死亡协程列表锁
    atomic_int ndead_co;                // 死亡协程数量

    // 空闲 P 列表
    struct list_head idle_p_list;      // 空闲 P 列表
    pthread_mutex_t idle_p_lock;       // 空闲 P 列表锁
    atomic_int npidle;                 // 空闲 P 数量
    
    // 全局状态
    atomic_bool stop_the_world;       // 停止所有协程标志
    
    // 协程 ID 分配
    atomic_size_t coid_gen;           // 协程 ID 生成器
};


// 常量定义
#define COMAXPROCS_DEFAULT 4          // 默认 P 数量
#define STACK_SIZE_DEFAULT (1024 * 1024) // 默认协程栈大小 32KB

#define P_RUNQ_SIZE_MAX 8 // P 的最大运行队列大小，超出会向全局队列里丢

#define P_SCHED_CHECK_INTERVAL 61 // P 调度检查间隔 (每 P_SCHED_CHECK_INTERVAL 次检查是否需要调整本地队列)

#define P_STEAL_TRIES 3 // 窃取工作尝试次数

#endif
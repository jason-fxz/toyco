#include "list.h"
#include "co.h"
#include "panic.h"
#include "internal.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdatomic.h>
#include <setjmp.h>

// this function is used to switch stack and start a function on the new stack
//   ! this function never return
static inline void
stack_switch_call(void *sp, void *entry, uintptr_t arg) {
    __asm__ volatile (
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

static struct scheduler g_sched = {0};

__thread struct M *current_m = NULL;
__thread struct P *current_p = NULL; 
__thread struct co *current_g = NULL;

static struct co main_co;

static void co_wrapper(struct co *g);
void co_schedule(void);

static size_t next_coid(void) {
    return atomic_fetch_add(&g_sched.coid_gen, 1);
}

// wrapper to handle coroutine execution
static void co_wrapper(struct co *g) {
    debug("co_wrapper: starting G%ld (%s)\n", g->coid, g->name);
    co_set_status(g, CO_RUNNING);
    
    // execute
    g->func(g->arg);
    
    // dead
    debug("co_wrapper: G%ld finished\n", g->coid);
    co_set_status(g, CO_DEAD);
    pthread_mutex_lock(&g_sched.dead_co_lock);
    list_add_tail(&g->node, &g_sched.dead_co_list);
    atomic_fetch_add(&g_sched.ndead_co, 1);
    pthread_mutex_unlock(&g_sched.dead_co_lock);

    
    // call waiters
    struct co *entry, *tmp;
    pthread_mutex_lock(&g->waiters_lock);
    list_for_each_entry_safe(entry, tmp, &g->waiters, node) {
        co_set_status(entry, CO_RUNABLE);
        
        assert(current_p);
        pthread_mutex_lock(&current_p->queue_lock);
        
        atomic_fetch_sub(&g->waitq_size, 1);
        list_move(&entry->node, &current_p->run_queue);
        atomic_fetch_add(&current_p->runq_size, 1);

        pthread_mutex_unlock(&current_p->queue_lock);
    }
    pthread_mutex_unlock(&g->waiters_lock);

    co_schedule();
}


// put g into P's local run queue
static void runq_put(struct P *p, struct co *g) {
    if (!p || !g) return;
    
    pthread_mutex_lock(&p->queue_lock);
    g->p = p;
    list_add_tail(&g->node, &p->run_queue);
    atomic_fetch_add(&p->runq_size, 1);
    pthread_mutex_unlock(&p->queue_lock);
    debug("runq_put: G%ld -> P%d\n", g->coid, p->id);
}

// get g from P's local run queue
static struct co* runq_get(struct P *p) {
    if (!p) return NULL;

    pthread_mutex_lock(&p->queue_lock);
    if (list_empty(&p->run_queue)) {
        pthread_mutex_unlock(&p->queue_lock);
        return NULL;
    }
    
    struct co *g = list_first_entry(&p->run_queue, struct co, node);
    list_del_init(&g->node);
    atomic_fetch_sub(&p->runq_size, 1);
    pthread_mutex_unlock(&p->queue_lock);
    
    assert(g != NULL);
    assert_msg(g->p == p, "g = %p\n", g); 
    debug("runq_get: G%ld (%s) <- P%d\n", g->coid, g->name, p->id);

    return g;
}

// put g into global run queue
static void global_runq_put(struct co *g) {
    if (!g) return;
    
    pthread_mutex_lock(&g_sched.global_runq_lock);
    list_add_tail(&g->node, &g_sched.global_runq);
    atomic_fetch_add(&g_sched.global_runq_size, 1);
    pthread_mutex_unlock(&g_sched.global_runq_lock);
    debug("global_runq_put: G%ld (%s)\n", g->coid, g->name);
}

// get g from global run queue
static struct co* global_runq_get(void) {
    pthread_mutex_lock(&g_sched.global_runq_lock);
    if (list_empty(&g_sched.global_runq)) {
        pthread_mutex_unlock(&g_sched.global_runq_lock);
        return NULL;
    }
    
    struct co *g = list_first_entry(&g_sched.global_runq, struct co, node);
    list_del_init(&g->node);
    atomic_fetch_sub(&g_sched.global_runq_size, 1);
    pthread_mutex_unlock(&g_sched.global_runq_lock);

    debug("global_runq_get: G%ld (%s)\n", g->coid, g->name);
    return g;
}

// 从其他P窃取协程
bool steal_work(struct P *thisp) {
    if (!thisp) return false;
    
    // for (int i = 0; i < g_sched.num_p; i++) {
    //     struct P *p = &g_sched.all_p[i];
    //     if (p == thisp || p->status != P_RUNNING) continue;
        
    //     int size = atomic_load(&p->runq_size);
    //     if (size <= 1) continue; // 不值得窃取
        
    //     pthread_mutex_lock(&p->queue_lock);
    //     if (!list_empty(&p->run_queue)) {
    //         struct co *g = list_first_entry(&p->run_queue, struct co, node);
    //         list_del(&g->node);
    //         atomic_fetch_sub(&p->runq_size, 1);
    //         pthread_mutex_unlock(&p->queue_lock);
            
    //         // 将窃取到的协程放入本地队列
    //         runq_put(thisp, g);
    //         debug("steal_work: P%d stole G%ld from P%d\n", thisp->id, g->coid, p->id);
    //         return true;
    //     }
    //     pthread_mutex_unlock(&p->queue_lock);
    // }
    return false;
}

// 寻找可运行的协程
struct co* find_runnable(struct P *p) {
    assert_msg(p != NULL, "find_runnable: P is NULL\n");
    debug("find_runnable: searching for runnable G in P%d\n", p->id);
    
    // 1. 检查本地队列
    struct co *g = runq_get(p);
    if (g) return g;
    
    // 2. 检查全局队列
    g = global_runq_get();
    if (g) return g;

    // 3. 工作窃取
    if (steal_work(p)) {
        g = runq_get(p);
        if (g) return g;
    }
    return NULL;
}

// 获取空闲的 P
static struct P* p_get_idle(void) {
    pthread_mutex_lock(&g_sched.idle_p_lock);
    if (list_empty(&g_sched.idle_p_list)) {
        pthread_mutex_unlock(&g_sched.idle_p_lock);
        return NULL;
    }
    
    struct P *p = list_first_entry(&g_sched.idle_p_list, struct P, idle_node);
    list_del_init(&p->idle_node);
    atomic_fetch_sub(&g_sched.npidle, 1);
    pthread_mutex_unlock(&g_sched.idle_p_lock);
    
    p->status = P_RUNNING;
    debug("p_get_idle: P%d acquired\n", p->id);
    return p;
}

// 将 P 放回空闲列表
static void p_put_idle(struct P *p) {
    if (!p) return;
    
    p->status = P_IDLE;
    p->m = NULL;
    
    pthread_mutex_lock(&g_sched.idle_p_lock);
    list_add_tail(&p->idle_node, &g_sched.idle_p_list);
    atomic_fetch_add(&g_sched.npidle, 1);
    pthread_mutex_unlock(&g_sched.idle_p_lock);
    
    debug("p_put_idle: P%d returned to idle\n", p->id);
}

// 生成新的 M ID
static int next_m_id(void) {
    return atomic_fetch_add(&g_sched.m_id_gen, 1);
}

// 创建新的 M
static struct M* m_create(void) {
    struct M *m = calloc(1, sizeof(struct M));
    if (!m) return NULL;
    
    m->id = next_m_id();

    atomic_store(&m->spinning, false);
    atomic_store(&m->blocked, false);
    INIT_LIST_HEAD(&m->node);
    
    debug("m_create: M%d created\n", m->id);
    return m;
}

// 销毁 M
static void m_destroy(struct M *m) {
    if (!m) return;
    free(m);
}


// 将 M 加入到调度器管理
static void m_add_to_scheduler(struct M *m) {
    pthread_mutex_lock(&g_sched.m_lock);
    list_add_tail(&m->node, &g_sched.all_m_list);
    atomic_fetch_add(&g_sched.num_m, 1);
    pthread_mutex_unlock(&g_sched.m_lock);
}

// M 的主循环
static void* m_main_loop(void *arg) {
    struct M *m = (struct M*)arg;
    current_m = m;
    
    debug("m_main_loop: M%d started\n", m->id);
    
    // 获取一个 P
    struct P *p = p_get_idle();
    if (!p) {
        debug("m_main_loop: M%d no idle P available\n", m->id);
        return NULL;
    }
    
    // bind M to P
    m->p = p;
    p->m = m;
    current_p = p;
    debug("m_main_loop: M%d bound to P%d\n", m->id, p->id);
    
    // M main loop
    while (!atomic_load(&g_sched.stop_the_world)) {
        int val = setjmp(m->sched_context);
        if (val == 0) { // 保存上下文，进调度
            co_schedule();
        } else {
            // 恢复到主循环上下文: 因为没有协程可以运行
            // TODO 睡眠队列
            // 当前 spin
            usleep(10000); // 10ms
        }
    }
    
    // 清理
    p_put_idle(p);
    current_m = NULL;
    current_p = NULL;
    current_g = NULL;
    
    debug("m_main_loop: M%d stopped\n", m->id);
    return NULL;
}


//===================================================================
// 调度器核心
//===================================================================

// 协程调度
void co_schedule(void) {
    struct M *m = current_m;
    struct P *p = current_p;
    
    if (!m || !p) {
        panic("g_schedule: no current M or P\n");
    }
    
    // 寻找下一个可运行的协程
    struct co *g = find_runnable(p);
    if (g) {
        current_g = g;
        g->m = m;
        g->p = p;
        m->cur_g = g;

        debug("M%d g_schedule: switching to G%ld (%s)\n", m->id, g->coid, g->name);

        if (co_get_status(g) == CO_NEW) {
            co_set_status(g, CO_RUNNING);
            stack_switch_call(g->stack + g->stack_size, co_wrapper, (uintptr_t)g);
        } else if (co_get_status(g) == CO_RUNABLE) {
            co_set_status(g, CO_RUNNING);
            longjmp(g->context, 1);
        } else {
            panic("g_schedule: G%ld (%s) is not in a runnable state\n", g->coid, g->name);
        }
    } else { // 没有可运行的协程，返回到 M 的主循环
        debug("M%d g_schedule: no runnable G found\n", m->id);
        longjmp(m->sched_context, 1);
    }
}

// 初始化调度器
void scheduler_init(void) {
    debug("scheduler_init: initializing scheduler\n");
    
    memset(&g_sched, 0, sizeof(g_sched));
    
    // 设置默认参数
    char *max_procs_env = getenv("COMAXPROCS");
    if (max_procs_env) {
        g_sched.num_p = atoi(max_procs_env);
        if (g_sched.num_p <= 0) {
            panic("COMAXPROCS must be a positive integer\n");
        }
    } else {
        g_sched.num_p = COMAXPROCS_DEFAULT; // 默认值
    }
    
    // 初始化全局队列
    INIT_LIST_HEAD(&g_sched.global_runq);
    pthread_mutex_init(&g_sched.global_runq_lock, NULL);
    atomic_store(&g_sched.global_runq_size, 0);
    
    // 初始化空闲 P 列表
    INIT_LIST_HEAD(&g_sched.idle_p_list);
    pthread_mutex_init(&g_sched.idle_p_lock, NULL);
    atomic_store(&g_sched.npidle, 0);
    
    // 初始化 M 队列和锁
    INIT_LIST_HEAD(&g_sched.all_m_list);
    pthread_mutex_init(&g_sched.m_lock, NULL);
    atomic_store(&g_sched.num_m, 0);
    atomic_store(&g_sched.m_id_gen, 1);
    
    // 初始化协程 ID 生成器
    atomic_store(&g_sched.coid_gen, 1);
    
    // 初始化死亡协程列表
    INIT_LIST_HEAD(&g_sched.dead_co_list);
    pthread_mutex_init(&g_sched.dead_co_lock, NULL);
    atomic_store(&g_sched.ndead_co, 0);

    // 创建 P 数组
    g_sched.all_p = calloc(g_sched.num_p, sizeof(struct P));
    if (!g_sched.all_p) {
        panic("failed to allocate P array\n");
    }
    
    for (int i = 0; i < g_sched.num_p; i++) {
        struct P *p = g_sched.all_p + i;
        p->id = i + 1;
        p->status = P_IDLE;

        INIT_LIST_HEAD(&p->run_queue);
        pthread_mutex_init(&p->queue_lock, NULL);
        atomic_store(&p->runq_size, 0);
        
        p_put_idle(p);
    }
    
    // 初始化主协程
    memset(&main_co, 0, sizeof(main_co));
    main_co.name = "main";
    co_set_status(&main_co, CO_RUNNING);
    main_co.coid = 0; // 主协程 ID 为 0
    INIT_LIST_HEAD(&main_co.waiters);
    INIT_LIST_HEAD(&main_co.node);
    pthread_mutex_init(&main_co.waiters_lock, NULL);
    
    current_g = &main_co;
    
    atomic_store(&g_sched.stop_the_world, false);
    
    debug("scheduler_init: scheduler initialized with %d P\n", g_sched.num_p);
}

// 启动调度器
void scheduler_start(void) {
    debug("scheduler_start: starting scheduler\n");
    
    // 创建初始的 M，数量等于 P 的数量
    for (int i = 0; i < g_sched.num_p; i++) {
        struct M *m = m_create();
        if (!m) {
            panic("failed to create M%d\n", i);
        }
        
        // 将 M 加入到调度器管理
        m_add_to_scheduler(m);
        
        // 启动 M 的线程
        if (pthread_create(&m->thread, NULL, m_main_loop, m) != 0) {
            panic("failed to create thread for M%d\n", m->id);
        }
    }
    
    debug("scheduler_start: started %d machines\n", g_sched.num_p);
}

// 停止调度器
void scheduler_stop(void) {
    debug("scheduler_stop: stopping scheduler\n");
    
    atomic_store(&g_sched.stop_the_world, true);
    
    // 等待所有 M 结束
    struct M *m, *tmp;
    pthread_mutex_lock(&g_sched.m_lock);
    list_for_each_entry_safe(m, tmp, &g_sched.all_m_list, node) {
        pthread_mutex_unlock(&g_sched.m_lock);
        pthread_join(m->thread, NULL);
        pthread_mutex_lock(&g_sched.m_lock);
        
        list_del(&m->node);
        m_destroy(m);
    }
    pthread_mutex_unlock(&g_sched.m_lock);
    
    // 清理 P 资源
    free(g_sched.all_p);

    // 清理 dead coroutine list
    struct co *dead_co, *tmp_co;
    list_for_each_entry_safe(dead_co, tmp_co, &g_sched.dead_co_list, node) {
        co_free(dead_co);
    }
    
    debug("scheduler_stop: scheduler stopped\n");
}

// 创建新协程
struct co* co_start(const char *name, void (*func)(void *), void *arg) {

    
    struct co *g = calloc(1, sizeof(struct co));
    if (!g) {
        panic("failed to allocate memory for coroutine\n");
        return NULL;
    }
    
    // 基本信息
    g->name = strdup(name);
    g->func = func;
    g->arg = arg;
    co_set_status(g, CO_NEW);
    g->coid = next_coid();
    
    // 初始化链表
    INIT_LIST_HEAD(&g->waiters);
    INIT_LIST_HEAD(&g->node);
    pthread_mutex_init(&g->waiters_lock, NULL);
    
    // 分配栈
    g->stack_size = STACK_SIZE_DEFAULT;
    g->stack = malloc(g->stack_size);
    if (!g->stack) {
        free(g->name);
        free(g);
        panic("failed to allocate stack for coroutine\n");
        return NULL;
    }
    
    debug("co_start: created G%ld (%s)\n", g->coid, g->name);
    
    // 主线程作为运行时实体，直接将协程放入全局队列让工作线程处理
    global_runq_put(g);
    
    return g;
}

// 协程让出 CPU
void co_yield(void) {
    struct co *g = current_g;
    assert(g != NULL);
    if (g == &main_co) { // 特判 main
        debug("WARN co_yield: main coroutine cannot yield\n");
        return;
    }
    assert(current_p != NULL);
    debug("co_yield: G%ld (%s) yielding\n", g->coid, g->name);
    g->status = CO_RUNABLE;
    if (setjmp(g->context) == 0) { // 保存当前上下文
        runq_put(current_p, g);
        co_schedule();
    } else { // 恢复执行
        debug("co_yield: G%ld (%s) resumed\n", g->coid, g->name);
    }
}

// 等待协程结束
void co_wait(struct co *co) {
    assert_msg(co != NULL, "co_wait: co is NULL\n");
    struct co *g = current_g; 
    assert(g != NULL);
    
    debug("co_wait: G%ld waiting for G%ld (%s)\n", g->coid, co->coid, co->name);
    if (co_get_status(co) == CO_DEAD) {
        debug("co_wait: G%ld already dead\n", co->coid);
        return;
    }
    // main 主线程特判，现在就忙等待 TODO： 唤醒
    if (g == &main_co) {
        debug("main co_wait: main coroutine cannot wait, busy waiting\n");
        while (co_get_status(co) != CO_DEAD) {
            usleep(1000); // 主线程忙等待
        }
        debug("main co_wait: main coroutine finished waiting for G%ld\n", co->coid);
        return;
    }

    debug("co_wait: G%ld adding to waiters of G%ld (%s)\n", g->coid, co->coid, co->name);
    // 将当前协程 g 加入 co 的等待列表
    pthread_mutex_lock(&co->waiters_lock);
    if (co_get_status(co) == CO_DEAD) { // co 已经死了 直接 return
        pthread_mutex_unlock(&co->waiters_lock);
        return; 
    }
    debug("FUCKING\n");
    assert(list_empty(&g->node)); 
    list_move(&g->node, &co->waiters);
    debug("FUCKING2\n");

    atomic_fetch_add(&co->waitq_size, 1);
    pthread_mutex_unlock(&co->waiters_lock);
    co_set_status(g, CO_WAITING);

    
    if (setjmp(g->context) == 0) { // 保存上下文并让出 CPU
        co_schedule();
    } else { // 被唤醒，继续执行
        debug("co_wait: G%ld resumed after waiting\n", g->coid);
    }
}

void co_free(struct co *co) {
    if (!co || co == &main_co) return; // 不释放主协程
    debug("co_free: freeing G%ld (%s)\n", co->coid, co->name);
    if (co->name) {
        free(co->name);
        co->name = NULL;
    }
    if (co->stack) {
        free(co->stack);
        co->stack = NULL;
    }
    // 从 g_sched.dead_co_list 中删除
    pthread_mutex_lock(&g_sched.dead_co_lock);
    list_del(&co->node);
    atomic_fetch_sub(&g_sched.ndead_co, 1);
    pthread_mutex_unlock(&g_sched.dead_co_lock);
    free(co);
}


__attribute__((constructor))
static void co_init(void) {
    debug("co_init: initializing coroutine library\n");
    scheduler_init();
    scheduler_start();
}

__attribute__((destructor))
static void co_cleanup(void) {
    debug("co_cleanup: cleaning up coroutine library\n");
    scheduler_stop();
}

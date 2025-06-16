#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdatomic.h>
#include <setjmp.h>
#include "list.h"
#include "co.h"
#include "panic.h"
#include "internal.h"
#include "utils.h"

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
__thread int is_main_thread = 0; // 是否是主线程

static struct co main_co;

static inline void __stack_check_canary(struct co *g) {
    assert(g != NULL);
    if (g == &main_co) return; // 主协程不检查栈
    assert(g->stack != NULL);
    // 检查栈底 canary
    uint64_t bottom_canary = *(uint64_t*)g->stack;
    if (bottom_canary != STACK_CANARY) {
        panic("stack_check_canary: G%ld (%s) stack bottom corruption detected! canary=0x%lx\n", 
              g->coid, g->name, bottom_canary);
    }
}
static void co_wrapper(struct co *g);
void co_schedule(void);

static size_t next_coid(void) {
    return atomic_fetch_add(&g_sched.coid_gen, 1);
}

// wrapper to handle coroutine execution
static void co_wrapper(struct co *g) {
    __stack_check_canary(g);
    
    debug("co_wrapper: starting G%ld (%s)\n", g->coid, g->name);
    
    // execute
    g->func(g->arg);
    
    debug("co_wrapper: G%ld finished\n", g->coid);
    __stack_check_canary(g);
    co_exit(); // dead handle (mark as dead and call waiters)
}

// put g into P's local run queue
static void local_runq_put(struct P *p, struct co *g) {
    if (!p || !g) return;
    
    pthread_mutex_lock(&p->queue_lock);
    g->p = p;
    list_add_tail(&g->node, &p->run_queue);
    p->runq_size++;
    pthread_mutex_unlock(&p->queue_lock);
    debug("local_runq_put: P%d <- G%ld (%s)\n", p->id, g->coid, g->name);
}

// get g from P's local run queue
static struct co* local_runq_get(struct P *p) {
    if (!p) return NULL;

    pthread_mutex_lock(&p->queue_lock);
    assert(p->runq_size == list_size(&p->run_queue));
    if (list_empty(&p->run_queue)) {
        pthread_mutex_unlock(&p->queue_lock);
        return NULL;
    }

    struct co *g = list_pop_front(&p->run_queue, struct co, node);
    p->runq_size--;
    assert(g->p == p);
    pthread_mutex_unlock(&p->queue_lock);
    
    assert(g != NULL);
    assert_msg(g->p == p, "g: %p G%ld (%s)\n", g, g->coid, g->name); 
    debug("local_runq_get: G%ld (%s) <- P%d (len=%d)\n", g->coid, g->name, p->id, p->runq_size);

    return g;
}

// put g into global run queue
static void global_runq_put(struct co *g) {
    assert(g != NULL);
    // assert(g->status == CO_RUNABLE || g->status == CO_NEW);
    g->p = NULL;
    pthread_mutex_lock(&g_sched.global_runq_lock);
    assert_msg(g_sched.global_runq_size == list_size(&g_sched.global_runq), "global_runq_get: g_sched.global_runq_size=%d, list_size=%d\n", 
               g_sched.global_runq_size, list_size(&g_sched.global_runq));
    if (g_sched.global_runq_size == 0) {
        assert(g_sched.global_runq.next == &g_sched.global_runq);
        assert(g_sched.global_runq.prev == &g_sched.global_runq);
    }
               
    list_add_tail(&g->node, &g_sched.global_runq);
    g_sched.global_runq_size++;

    assert_msg(g_sched.global_runq_size == list_size(&g_sched.global_runq), "global_runq_put: g_sched.global_runq_size=%d, list_size=%d\n", g_sched.global_runq_size, list_size(&g_sched.global_runq));
    debug("global_runq_put: G%ld (%s)\n", g->coid, g->name);
    pthread_mutex_unlock(&g_sched.global_runq_lock);
}

// 从全局运行队列获取 max 个协程，第一个会直接返回，其余插入到 P 的本地队列 
// max = 0 时自动计算获取个数, 
// !! 需要在调用这个函数前对 g_sched.global_runq_lock 上锁
static struct co* global_runq_get(struct P *p, int max) {
    assert(p == current_p);
    assert_msg(g_sched.global_runq_size == list_size(&g_sched.global_runq), "global_runq_get: g_sched.global_runq_size=%d, list_size=%d\n", 
               g_sched.global_runq_size, list_size(&g_sched.global_runq));
    if (g_sched.global_runq_size == 0) {
        return NULL;
    }
    int n = g_sched.global_runq_size / g_sched.nproc + 1; // 平均
    if (n > g_sched.global_runq_size) n = g_sched.global_runq_size;

    if (max > 0 && n > max) n = max; 
    if (n > P_RUNQ_SIZE_MAX / 2) n = P_RUNQ_SIZE_MAX / 2;
    assert(n > 0);

    if (p->runq_size == P_RUNQ_SIZE_MAX) {
        assert(n == 1);
        // fprintf(stderr, "FUCK\n");
    }
    if (max != 0) {
        assert_msg(n == max, "global_runq_get: n = %d, max = %d\n", n, max);
    }
    g_sched.global_runq_size -= n;
    assert(g_sched.global_runq_size >= 0);
    
    struct co *g1 = list_pop_front(&g_sched.global_runq, struct co, node);
    assert(g1 != NULL);
    assert(g1->p == NULL);
    debug("global_runq_get: G%ld (%s) <- global run queue, n = %d\n", 
          g1->coid, g1->name, n);
    n--;
    
    if (n > 0) {
        pthread_mutex_lock(&p->queue_lock);
        for (; n > 0; n--) {
            struct co *g = list_pop_front(&g_sched.global_runq, struct co, node);
            assert_msg(g != NULL, "n = %d g_sched.global_runq_size=%d\n", n, g_sched.global_runq_size);
            assert(g->p == NULL);
            list_add_tail(&g->node, &p->run_queue);
            p->runq_size++;
            g->p = p;
        }
        pthread_mutex_unlock(&p->queue_lock);
    }

    if (g_sched.global_runq_size == 0) {
        assert(g_sched.global_runq.next == &g_sched.global_runq);
        assert(g_sched.global_runq.prev == &g_sched.global_runq);
    }
    
    assert_msg(g_sched.global_runq_size == list_size(&g_sched.global_runq), "global_runq_get: g_sched.global_runq_size=%d, list_size=%d\n", 
               g_sched.global_runq_size, list_size(&g_sched.global_runq));
    return g1;
}

// 将 g 放入 runq，优先放本地队列，若本地队列不存在/已满，则放入全局队列
static void runq_put(struct P *p, struct co *g) {
    assert(g != NULL);
    // assert(g->status == CO_RUNABLE || g->status == CO_NEW);
    if (p == NULL) {
        global_runq_put(g);
        return;
    }
    pthread_mutex_lock(&p->queue_lock);
    if (p->runq_size >= P_RUNQ_SIZE_MAX) {
        pthread_mutex_unlock(&p->queue_lock);
        global_runq_put(g);
        return;
    }
    g->p = p;
    p->runq_size++;
    list_add_tail(&g->node, &p->run_queue);
    debug("runq_put: P%d <- G%ld (%s) (len=%d)\n", p->id, g->coid, g->name, p->runq_size);
    pthread_mutex_unlock(&p->queue_lock);
}
// 从其他P窃取协程
struct co* steal_work(struct P *p) {
    if (!p) return NULL;
    
    debug("steal_work: P%d trying to steal work\n", p->id);

    for (int i = 0; i < P_STEAL_TRIES; ++i) {
        // random enum P
        int nproc = g_sched.nproc;
        int perm[nproc];
        for (int i = 0; i < nproc; ++i) perm[i] = i;
        for (int i = nproc - 1; i > 0; --i) {
            int j = fast_rand() % (i + 1);
            int tmp = perm[i];
            perm[i] = perm[j];
            perm[j] = tmp;
        }

        for (int k = 0; k < nproc; ++k) {
            struct P *victim = &g_sched.all_p[perm[k]];
            if (victim == p || victim->status != P_RUNNING) continue;

            pthread_mutex_lock(&victim->queue_lock);
            int n = victim->runq_size;
            if (n <= 1) {
                pthread_mutex_unlock(&victim->queue_lock);
                continue;
            }
            n -= n / 2;
            assert(n > 0);
            victim->runq_size -= n;
            struct co *g1 = list_pop_back(&victim->run_queue, struct co, node);
            assert(g1 != NULL);
            debug("steal_work: P%d stole G%ld (%s) from P%d, n = %d\n", 
                  p->id, g1->coid, g1->name, victim->id, n);
            n--;
            for (; n > 0; n--) {
                struct co *g = list_pop_back(&victim->run_queue, struct co, node);
                assert(g != NULL);
                local_runq_put(p, g);
            }
            pthread_mutex_unlock(&victim->queue_lock);
            return g1;
        }
    }  
    return NULL;
}

// 寻找可运行的协程
struct co* find_runnable(struct P *p) {
    assert_msg(p != NULL, "find_runnable: P is NULL\n");
    debug("find_runnable: searching for runnable G in P%d (tick = %ld)\n", p->id, p->sched_tick);
    struct co *g = NULL;
    assert(p == current_p);
    
    // 0. 一定调度次数后检查全局队列
    if (p->sched_tick % P_SCHED_CHECK_INTERVAL == 0) {
        bool flag = false;
        pthread_mutex_lock(&p->queue_lock);
        flag = (p->runq_size < P_RUNQ_SIZE_MAX);
        pthread_mutex_unlock(&p->queue_lock);
        if (flag) {
            debug("find_runnable: P%d checking global run queue\n", p->id);
            assert(p->runq_size == list_size(&p->run_queue));
            assert(p->runq_size <= P_RUNQ_SIZE_MAX);
    
            pthread_mutex_lock(&g_sched.global_runq_lock);
            g = global_runq_get(p, 1);
            pthread_mutex_unlock(&g_sched.global_runq_lock);
            if (g) {
                return g;
            }
        }
    }
    debug("check local_runq\n");
    // 1. 检查本地队列
    g = local_runq_get(p);
    if (g) return g;
    debug("check global_runq\n");
    // 2. 检查全局队列 (自动选择窃取数量) 
    pthread_mutex_lock(&g_sched.global_runq_lock);
    g = global_runq_get(p, 0);
    pthread_mutex_unlock(&g_sched.global_runq_lock);
    if (g) return g;

    // 3. 工作窃取
    // g = steal_work(p);
    // if (g) return g;

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
    assert(p->m == m);
    assert(m->p == p);
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


// 协程调度 
// 这段函数可以在任意栈上调用，只是临时借用一下栈，任何出口都不会返回
void co_schedule(void) {
    struct M *m = current_m;
    struct P *p = current_p;
    current_g = NULL; // 进入 schedule 时清除 current_g 标记
    if (!m || !p) {
        panic("g_schedule: no current M or P\n");
    }

    // 寻找下一个可运行的协程
    p->sched_tick++;    
    struct co *g = find_runnable(p);
    if (g) {
        current_g = g;
        g->p = p;
        m->cur_g = g;

        debug("M%d g_schedule: switching to G%ld (%s)\n", m->id, g->coid, g->name);

        __stack_check_canary(g);

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
        g_sched.nproc = atoi(max_procs_env);
        if (g_sched.nproc <= 0) {
            panic("COMAXPROCS must be a positive integer\n");
        }
    } else {
        g_sched.nproc = COMAXPROCS_DEFAULT; // 默认值
    }
    
    // 初始化全局队列
    INIT_LIST_HEAD(&g_sched.global_runq);
    pthread_mutex_init(&g_sched.global_runq_lock, NULL);
    g_sched.global_runq_size = 0;
    
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
    g_sched.ndead_co = 0;

    // 创建 P 数组
    g_sched.all_p = calloc(g_sched.nproc, sizeof(struct P));
    if (!g_sched.all_p) {
        panic("failed to allocate P array\n");
    }
    
    for (int i = 0; i < g_sched.nproc; i++) {
        struct P *p = g_sched.all_p + i;
        p->id = i + 1;
        p->status = P_IDLE;

        INIT_LIST_HEAD(&p->run_queue);
        pthread_mutex_init(&p->queue_lock, NULL);
        p->runq_size = 0;

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
    main_co.waitq_size = 0;
    
    current_g = &main_co;
    
    atomic_store(&g_sched.stop_the_world, false);
    
    debug("scheduler_init: scheduler initialized with %d P\n", g_sched.nproc);

    is_main_thread = 1;
}

// 启动调度器
void scheduler_start(void) {
    debug("scheduler_start: starting scheduler\n");
    
    // 创建初始的 M，数量等于 P 的数量
    for (int i = 0; i < g_sched.nproc; i++) {
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
    
    debug("scheduler_start: started %d machines\n", g_sched.nproc);
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
    g->waitq_size = 0;
    g->p = NULL; // 初始时没有绑定到 P
    
    // 分配栈
    g->stack_size = STACK_SIZE_DEFAULT;
    size_t alloc_size = g->stack_size;
    g->stack = malloc(alloc_size);
    if (!g->stack) {
        free(g->name);
        free(g);
        panic("failed to allocate stack for coroutine\n");
        return NULL;
    }
    
    // // 在栈底部添加 canary 值用于检测栈溢出
    *(uint64_t*)g->stack = 0xDEADBEEFCAFEBABE;

    debug("co_start: created G%ld (%s)\n", g->coid, g->name);

    runq_put(current_p, g);

    return g;
}

// 退出当前协程
void co_exit() {
    __stack_check_canary(current_g);
    struct co *g = current_g;
    assert(g != NULL);
    if (g == &main_co) { // 特判 main 协程
        debug("co_exit: main coroutine cannot exit\n");
        return;
    }    
    // dead
    co_set_status(g, CO_DEAD);
    pthread_mutex_lock(&g_sched.dead_co_lock);
    assert(list_empty(&g->node));
    list_add_tail(&g->node, &g_sched.dead_co_list);
    g_sched.ndead_co++;
    pthread_mutex_unlock(&g_sched.dead_co_lock);

    // call waiters
    struct co *entry, *tmp;
    pthread_mutex_lock(&g->waiters_lock);
    list_for_each_entry_safe(entry, tmp, &g->waiters, node) {
        list_del_init(&entry->node);
        g->waitq_size--;
        co_set_status(entry, CO_RUNABLE);
        
        assert(current_p);
        runq_put(current_p, entry); // 将等待的协程放回运行队列
    }
    pthread_mutex_unlock(&g->waiters_lock);
    __stack_check_canary(current_g);
    co_schedule();
}

// 协程让出 CPU
void co_yield(void) {
    __stack_check_canary(current_g);
    
    struct co *g = current_g;
    assert(g != NULL);
    if (g == &main_co) { // 特判 main
        debug("WARN co_yield: main coroutine cannot yield\n");
        return;
    }


    assert(current_p != NULL);
    // debug("co_yield: G%ld (%s) yielding\n", g->coid, g->name);
    co_set_status(g, CO_RUNABLE);
    if (setjmp(g->context) == 0) { // 保存当前上下文
        runq_put(current_p, g);
        co_schedule();
    } else { // 恢复执行
        __stack_check_canary(current_g);
        // debug("co_yield: G%ld (%s) resumed\n", g->coid, g->name);
    }
}

// 等待协程结束
void co_wait(struct co *co) {
    assert_msg(co != NULL, "co_wait: co is NULL\n");
    struct co *g = current_g; 
    assert(g != NULL);
    
    if (g != &main_co) {
        __stack_check_canary(current_g);
    }

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
    assert(list_empty(&g->node)); 
    list_move(&g->node, &co->waiters);
    co->waitq_size++;
    co_set_status(g, CO_WAITING);
    pthread_mutex_unlock(&co->waiters_lock);

    
    if (setjmp(g->context) == 0) { // 保存上下文并让出 CPU
        co_schedule();
    } else { // 被唤醒，继续执行
        __stack_check_canary(g);
        // debug("co_wait: G%ld resumed after waiting\n", g->coid);
    }
}

// 手动释放协程资源
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
    g_sched.ndead_co--;
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
    if (is_main_thread) {
        debug("co_cleanup: cleaning up coroutine library\n");
        scheduler_stop();
    } 
}

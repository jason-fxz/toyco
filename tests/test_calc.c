#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include <co.h>

// ====== 配置参数 ======
#define MAX_NODES 10000  // 最大协程数量
#define MAX_DEPS     8   // 每个节点最多依赖多少个
#define YIELD_COUNT  50   // 每个任务 yield 几次
#define STEP_TIME_MS 1  // 每个阶段耗时（ms）

// ====== 图边记录结构 ======
typedef struct {
    char from[32];
    char to[32];
} Edge;

Edge graph[MAX_NODES * MAX_DEPS];
int edge_count = 0;
pthread_mutex_t graph_lock = PTHREAD_MUTEX_INITIALIZER;

void record_edge(const char *from, const char *to) {
    pthread_mutex_lock(&graph_lock);
    if (edge_count < MAX_NODES * MAX_DEPS) {
        strncpy(graph[edge_count].from, from, 31);
        strncpy(graph[edge_count].to, to, 31);
        edge_count++;
    }
    pthread_mutex_unlock(&graph_lock);
}

void export_graph(const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        perror("fopen");
        return;
    }
    fprintf(f, "digraph ComputationGraph {\n");
    for (int i = 0; i < edge_count; ++i) {
        fprintf(f, "  \"%s\" -> \"%s\";\n", graph[i].from, graph[i].to);
    }
    fprintf(f, "}\n");
    fclose(f);
}

// ====== 模拟空转运行 ======
void busy_work(int duration_ms) {
    clock_t start = clock();
    clock_t end = start + duration_ms * CLOCKS_PER_SEC / 1000;
    while (clock() < end);
}

// ====== 任务结构 ======
typedef struct task_arg {
    const char *name;
    int dep_count;
    struct co *deps[MAX_DEPS];
} task_arg_t;

// ====== 协程任务函数 ======
void task(void *arg_) {
    task_arg_t *arg = (task_arg_t *)arg_;

    
    for (int i = 0; i < arg->dep_count; ++i) {
        struct co *dep = arg->deps[i];
        if (dep) {
            co_wait(dep);
        }
    }
    for (int step = 0; step < YIELD_COUNT; ++step) {
        busy_work(STEP_TIME_MS);
        co_yield();
    }

    printf("[%s] finished\n", arg->name);
}

// ====== 主程序入口 ======
int main() {
    int N = 1000;  // 协程数量
    int K = 3;     // 每个协程最多依赖多少个

    struct co *cos[MAX_NODES];
    task_arg_t args[MAX_NODES];
    char name_pool[MAX_NODES][32];

    srand(114514);

    // 构造 DAG 并启动协程
    for (int i = 0; i < N; ++i) {
        snprintf(name_pool[i], sizeof(name_pool[i]), "T%d", i);
        args[i].name = name_pool[i];

        int dep_count = (i == 0) ? 0 : rand() % (K + 1);
        if (dep_count > i) dep_count = i;
        args[i].dep_count = dep_count;

        for (int j = 0; j < dep_count; ++j) {
            int dep_idx = rand() % i;
            args[i].deps[j] = cos[dep_idx];
            // T[dep_idx] -> T[i]
            record_edge(name_pool[dep_idx], name_pool[i]);
        }

        cos[i] = co_start(args[i].name, task, &args[i]);
    }

    // 等待所有协程完成
    for (int i = 0; i < N; ++i) {
        co_wait(cos[i]);
    }

    // 输出计算图
    export_graph("graph.out");

    printf("All tasks finished. Graph written to graph.out\n");
    return 0;
}
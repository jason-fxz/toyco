#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "co.h"

#define LAYERS 10       // 层数
#define WIDTH  1000      // 每层宽度
#define WORK   50000    // 每个节点计算量

struct node_arg {
    int layer;
    int pos;
    int dep_num;
    struct co **deps;
    double result;
};

void do_some_work(int idx, double *result) {
    double acc = idx;
    for (int i = 0; i < WORK; ++i)
        acc = acc * 1.0000001 + 0.5;
    *result = acc;
}

void dag_worker(void *arg) {
    struct node_arg *narg = (struct node_arg*)arg;
    for (int i = 0; i < narg->dep_num; ++i)
        co_wait(narg->deps[i]);
    do_some_work(narg->layer * WIDTH + narg->pos, &narg->result);
    free(narg->deps);
    free(narg);
}

int main() {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    struct co *nodes[LAYERS][WIDTH] = {{0}};

    // 创建所有节点
    for (int l = 0; l < LAYERS; ++l) {
        for (int w = 0; w < WIDTH; ++w) {
            struct node_arg *arg = malloc(sizeof(struct node_arg));
            arg->layer = l;
            arg->pos = w;

            // 0层没有依赖，其它层依赖上一层所有节点（可改为部分依赖）
            if (l == 0) {
                arg->dep_num = 0;
                arg->deps = NULL;
            } else {
                arg->dep_num = WIDTH;
                arg->deps = malloc(sizeof(struct co*) * WIDTH);
                for (int k = 0; k < WIDTH; ++k)
                    arg->deps[k] = nodes[l-1][k];
            }
            nodes[l][w] = co_start("dag", dag_worker, arg);
        }
    }

    // 等待最后一层全部节点完成
    for (int w = 0; w < WIDTH; ++w) {
        co_wait(nodes[LAYERS-1][w]);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double sec = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("DAG layers: %d, width: %d, total coroutines: %d\n", LAYERS, WIDTH, LAYERS*WIDTH);
    printf("Total time: %.6f s\n", sec);
    return 0;
}

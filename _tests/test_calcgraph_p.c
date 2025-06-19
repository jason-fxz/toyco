#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#define LAYERS 10
#define WIDTH  1000
#define WORK   50000

pthread_barrier_t barriers[LAYERS]; // 每层一个 barrier

struct node_arg {
    int layer;
    int pos;
    double result;
};

void do_some_work(int idx, double *result) {
    double acc = idx;
    for (int i = 0; i < WORK; ++i)
        acc = acc * 1.0000001 + 0.5;
    *result = acc;
}

void *dag_worker(void *arg_) {
    struct node_arg *narg = (struct node_arg*)arg_;
    int l = narg->layer;
    if (l > 0)
        pthread_barrier_wait(&barriers[l-1]);
    do_some_work(l * WIDTH + narg->pos, &narg->result);
    pthread_barrier_wait(&barriers[l]);
    free(narg);
    return NULL;
}

int main() {
    struct timespec start, end;
    pthread_t nodes[LAYERS][WIDTH];

    for (int l = 0; l < LAYERS; ++l)
        pthread_barrier_init(&barriers[l], NULL, WIDTH);

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int l = 0; l < LAYERS; ++l) {
        for (int w = 0; w < WIDTH; ++w) {
            struct node_arg *arg = malloc(sizeof(struct node_arg));
            arg->layer = l;
            arg->pos = w;
            pthread_create(&nodes[l][w], NULL, dag_worker, arg);
        }
    }

    // join 所有线程
    for (int l = 0; l < LAYERS; ++l)
        for (int w = 0; w < WIDTH; ++w)
            pthread_join(nodes[l][w], NULL);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double sec = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("pthread DAG layers: %d, width: %d, total threads: %d\n", LAYERS, WIDTH, LAYERS*WIDTH);
    printf("Total time: %.6f s\n", sec);

    for (int l = 0; l < LAYERS; ++l)
        pthread_barrier_destroy(&barriers[l]);

    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "co.h"

// #define PRINT

#define N_CO 1000         // 协程数
#define ITER 500    // 每协程内部迭代次数

struct co *cor[N_CO], *corv[N_CO];

// 简单协程任务
void co_task(void *arg) {
    int id = (int)(intptr_t)arg;

    for (int i = 0; i < ITER; i++) {
        #ifdef PRINT
        printf("[co %d] iteration %d\n", id, i);
        #endif
        co_yield();
    }
}

void co_taskv(void *arg) {
    int id = (int)(intptr_t)arg;

    for (int i = 0; i < ITER; i++) {
        #ifdef PRINT
        printf("[cov %d] iteration %d\n", id, i);
        #endif
        co_resume(cor[rand() % N_CO]);
    }
}

int main() {

    for (int i = 0; i < N_CO; i++) {
        cor[i] = co_start("worker", co_task, (void*)(intptr_t)i);
        if (!cor[i]) {
            fprintf(stderr, "co_start failed at %d\n", i);
            exit(1);
        }
    }

    for (int i = 0; i < N_CO; i++) {
        corv[i] = co_start("worker_v", co_taskv, (void*)(intptr_t)i);
        if (!corv[i]) {
            fprintf(stderr, "co_start failed at %d\n", i);
            exit(1);
        }
    }


    for (int i = 0; i < N_CO; i++) {
        co_wait(cor[i]);
    }

    for (int i = 0; i < N_CO; i++) {
        co_wait(corv[i]);
    }
    printf("All coroutines finished without crash\n");
    return 0;
}

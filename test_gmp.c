#include <stdio.h>
#include <unistd.h>
#include "co.h"

#define ITER 5
#define MAX_CO 4

void simple_work(void *arg) {
    int id = (int)(size_t)arg; // 将 void* 转换为 int
    for (int i = 0; i < ITER; i++) {
        printf("[%d] Working %d\n", id, i);
        usleep(1000);
        co_yield();
    }
    printf("[%d] Done!\n", id);
}

int main() {
    printf("Starting G-P-M scheduler test\n");
    struct co *g[MAX_CO];


    for (int i = 0; i < MAX_CO; i++) {
        char name[20];
        snprintf(name, sizeof(name), "Worker-%d", i + 1);
        g[i] = co_start(name, simple_work, (void*)(size_t)i);
    }

    printf("Created %d coroutines\n", MAX_CO);

    co_yield();
    // 等待协程完成
    for (int i = 0; i < MAX_CO; i++) {
        co_wait(g[i]);
    }
    
    printf("All coroutines completed\n");
    return 0;
}

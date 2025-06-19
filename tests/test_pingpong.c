#include <stdio.h>
#include "co.h"

#define ITER 5000000

volatile int flag = 0;

void ping(void *arg) {
    for (int i = 0; i < ITER; ++i) {
        while (__atomic_load_n(&flag, __ATOMIC_ACQUIRE) != 0) co_yield();
        __atomic_store_n(&flag, 1, __ATOMIC_RELEASE);
        co_yield();
    }
}

void pong(void *arg) {
    for (int i = 0; i < ITER; ++i) {
        while (__atomic_load_n(&flag, __ATOMIC_ACQUIRE) != 1) co_yield();
        __atomic_store_n(&flag, 0, __ATOMIC_RELEASE);
        co_yield();
    }
}

int main() {
    struct co *c1 = co_start("ping", ping, NULL);
    struct co *c2 = co_start("pong", pong, NULL);
    co_wait(c1); 
    co_wait(c2);
    printf("Ping-pong test done.\n");
    return 0;
}

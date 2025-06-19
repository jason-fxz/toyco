#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

#define BUF_SIZE 10
#define N_PRODUCE 100
#define N_PRODUCER 400
#define N_CONSUMER 400

int buffer[BUF_SIZE];
int head = 0, tail = 0;
int count = 0;

pthread_mutex_t mutex;
pthread_cond_t cond_empty;
pthread_cond_t cond_full;
volatile int dummy_sink;

void put(int val) {
    buffer[tail] = val;
    tail = (tail + 1) % BUF_SIZE;
    count++;
    dummy_sink = val; // 防止编译器优化
}

int get() {
    int val = buffer[head];
    head = (head + 1) % BUF_SIZE;
    count--;
    dummy_sink = val; // 防止编译器优化
    return val;
}

void* producer(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < N_PRODUCE; ++i) {
        pthread_mutex_lock(&mutex);
        while (count == BUF_SIZE)
            pthread_cond_wait(&cond_empty, &mutex);

        int val = id * 1000 + i;
        put(val);
        #ifdef PRINT
        printf("[P%d] produce %d\n", id, val);
        #endif

        pthread_cond_signal(&cond_full);
        pthread_mutex_unlock(&mutex);
        // pthread_yield(); // 一般不需要显式让出CPU
    }
    return NULL;
}

void* consumer(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < N_PRODUCE * N_PRODUCER / N_CONSUMER; ++i) {
        pthread_mutex_lock(&mutex);
        while (count == 0)
            pthread_cond_wait(&cond_full, &mutex);

        int val = get();
        #ifdef PRINT
        printf("        [C%d] consume %d\n", id, val);
        #endif

        pthread_cond_signal(&cond_empty);
        pthread_mutex_unlock(&mutex);
        // pthread_yield(); // 一般不需要显式让出CPU
    }
    return NULL;
}

int main() {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond_empty, NULL);
    pthread_cond_init(&cond_full, NULL);

    pthread_t producers[N_PRODUCER];
    pthread_t consumers[N_CONSUMER];

    for (int i = 0; i < N_PRODUCER; ++i) {
        pthread_create(&producers[i], NULL, producer, (void*)(intptr_t)i);
    }

    for (int i = 0; i < N_CONSUMER; ++i) {
        pthread_create(&consumers[i], NULL, consumer, (void*)(intptr_t)i);
    }

    for (int i = 0; i < N_PRODUCER; ++i) {
        pthread_join(producers[i], NULL);
    }

    for (int i = 0; i < N_CONSUMER; ++i) {
        pthread_join(consumers[i], NULL);
    }

    printf("Finished. Final buffer count = %d\n", count);

    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond_empty);
    pthread_cond_destroy(&cond_full);

    // time cost
    clock_gettime(CLOCK_MONOTONIC, &end);
    double sec = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("Total time: %.3f s\n", sec);
    return 0;
}

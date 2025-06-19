#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>

#define BUF_SIZE 10
#define N_PRODUCE 100
#define N_PRODUCER 400
#define N_CONSUMER 400

int buffer[BUF_SIZE];
int head = 0, tail = 0;
int count = 0;

sem_t sem_empty;
sem_t sem_full;
sem_t sem_mutex;
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
        sem_wait(&sem_empty);
        sem_wait(&sem_mutex);

        int val = id * 1000 + i;
        put(val);
#ifdef PRINT
        printf("[P%d] produce %d\n", id, val);
#endif

        sem_post(&sem_mutex);
        sem_post(&sem_full);
        // sched_yield(); // 可选，通常不需要
    }
    return NULL;
}

void* consumer(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < N_PRODUCE * N_PRODUCER / N_CONSUMER; ++i) {
        sem_wait(&sem_full);
        sem_wait(&sem_mutex);

        int val = get();
#ifdef PRINT
        printf("        [C%d] consume %d\n", id, val);
#endif

        sem_post(&sem_mutex);
        sem_post(&sem_empty);
        // sched_yield(); // 可选
    }
    return NULL;
}

int main() {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    sem_init(&sem_empty, 0, BUF_SIZE);
    sem_init(&sem_full, 0, 0);
    sem_init(&sem_mutex, 0, 1);

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

    sem_destroy(&sem_empty);
    sem_destroy(&sem_full);
    sem_destroy(&sem_mutex);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double sec = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("Total time: %.3f s\n", sec);
    return 0;
}

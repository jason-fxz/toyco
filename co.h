/*
 * co.h
 * User Interface for Coroutine library
 */

#ifndef CO_H
#define CO_H

#include <pthread.h>
#include "list.h"

struct co* co_start(const char *name, void (*func)(void *), void *arg);
void co_yield();
void co_wait(struct co *co);
void co_free(struct co *co);
void co_exit(void);
void co_resume(struct co *co);

// semaphore
struct co_sem {
    int count;
    struct list_head waiters;
    pthread_spinlock_t lock;
};
void co_sem_init(struct co_sem *sem, int initial);
void co_sem_wait(struct co_sem *sem);
void co_sem_post(struct co_sem *sem);

#endif
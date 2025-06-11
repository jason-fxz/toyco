/*
 * co.h
 * User Interface for Coroutine library
 */

#ifndef CO_H
#define CO_H

struct co* co_start(const char *name, void (*func)(void *), void *arg);
void co_yield();
void co_wait(struct co *co);
void co_free(struct co *co);
void co_exit(void);

#endif
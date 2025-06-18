# ex-toyco

goroutine like toy coroutine lib for c
support multiple coroutine on multiple threads

## APIs

```c
struct co* co_start(const char *name, void (*func)(void *), void *arg);
void       co_yield();
void       co_wait(struct co *co);
void       co_free(struct co *co);
void       co_exit(void);
```

1. `co_start(name, func, arg)` 创建一个新的协程，并返回一个指向 `struct co` 的指针 (类似于 `pthread_create`)。
   - 新创建的协程从函数 `func` 开始执行，并传入参数 `arg`。新创建的协程不会立即执行，而是调用 `co_start` 的协程继续执行。
   - `co_start` 返回的 `struct co` 指针需要 `malloc()` 分配内存。
2. `co_wait(co)` 表示当前协程需要等待，直到 `co` 协程的执行完成才能继续执行 (类似于 `pthread_join`)。
   - 允许多个协程等待同一个协程。
   - `co` 结束时不会自动释放 `co` 占用的内存, `main` 函数结束时会释放所有协程占用的内存。
3. `co_yield()` 实现协程的切换。协程运行后一直在 CPU 上执行，直到 `func` 函数返回或调用 `co_yield` 使当前运行的协程暂时放弃执行。由调度器选择下一个要执行的协程。
4. `co_free(co)` 显示释放 `co` 协程占用的内存。注意，`co_free` 只能在 `co` 协程结束后调用。
5. `co_exit()` 结束当前协程的执行。
6. `main` 函数的执行也是一个协程，因此可以在 `main` 中调用 `co_yield` 或 `co_wait`。`main` 函数返回后，无论有多少协程，进程都将直接终止。

### semaphore

```c
void co_sem_init(struct co_sem *sem, int initial);
void co_sem_wait(struct co_sem *sem);
void co_sem_post(struct co_sem *sem);
```

协程信号量，类似于 POSIX 信号量。

1. `co_sem_init(sem, initial)` 初始化信号量 `sem`，初始值为 `initial`
2. `co_sem_wait(sem)` 阻塞当前协程，直到信号量 `sem` 的值大于 0，然后将 `sem` 的值减 1。
3. `co_sem_post(sem)` 将信号量 `sem` 的值加 1，并唤醒一个等待该信号量的协程。

## Examples

创建两个 (永不结束的) 协程，分别打印 a 和 b，交替执行。将会看到随机的 ab 交替出现的序列，例如 ababbabaaaabbaa...

```c
#include <stdio.h>
#include "co.h"

void entry(void *arg) {
    while (1) {
        printf("%s", (const char *)arg);
        co_yield();
    }
}

int main() {
    struct co *co1 = co_start("co1", entry, "a");
    struct co *co2 = co_start("co2", entry, "b");
    co_wait(co1); // never returns
    co_wait(co2);
}
```


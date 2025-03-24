# toyco

toy coroutine lib for c

## APIs

```c
struct co *co_start(const char *name, void (*func)(void *), void *arg);
void       co_yield();
void       co_wait(struct co *co);
```

1. `co_start(name, func, arg)` 创建一个新的协程，并返回一个指向 `struct co` 的指针 (类似于 `pthread_create`)。
   - 新创建的协程从函数 `func` 开始执行，并传入参数 `arg`。新创建的协程不会立即执行，而是调用 `co_start` 的协程继续执行。
   - `co_start` 返回的 `struct co` 指针需要 `malloc()` 分配内存。
2. `co_wait(co)` 表示当前协程需要等待，直到 `co` 协程的执行完成才能继续执行 (类似于 `pthread_join`)。
   - 允许多个协程等待同一个协程。
   - `co` 结束时不会释放 `co` 占用的内存, `main` 函数结束时会释放所有协程占用的内存。
3. `co_yield()` 实现协程的切换。协程运行后一直在 CPU 上执行，直到 `func` 函数返回或调用 `co_yield` 使当前运行的协程暂时放弃执行。`co_yield` 时若系统中有多个可运行的协程时 (包括当前协程)，你随机选择下一个系统中可运行的协程。
4. `main` 函数的执行也是一个协程，因此可以在 `main` 中调用 `co_yield` 或 `co_wait`。`main` 函数返回后，无论有多少协程，进程都将直接终止。

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

两个协程会交替执行，共享 counter 变量：字母是随机的 (a 或 b)，数字则从 1 到 10 递增。

```plaintext
b[1] a[2] b[3] b[4] a[5] b[6] b[7] a[8] a[9] a[10] Done
```

```cpp
#include <stdio.h>
#include "co.h"

int count = 1; // 协程之间共享

void entry(void *arg) {
    for (int i = 0; i < 5; i++) {
        printf("%s[%d] ", (const char *)arg, count++);
        co_yield();
    }
}

int main() {
    struct co *co1 = co_start("co1", entry, "a");
    struct co *co2 = co_start("co2", entry, "b");
    co_wait(co1);
    co_wait(co2);
    printf("Done\n");
}
```

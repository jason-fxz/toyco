#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include "co.h"

// 基础协程功能测试

void simple_coroutine(void *arg) {
    int id = (int)(size_t)arg;
    printf("Coroutine %d: Starting\n", id);
    
    for (int i = 0; i < 3; i++) {
        printf("Coroutine %d: Iteration %d\n", id, i);
        co_yield();
    }
    
    printf("Coroutine %d: Finished\n", id);
}

void test_basic_yield() {
    printf("\n=== Test Basic Yield ===\n");
    
    struct co *co1 = co_start("test1", simple_coroutine, (void*)(size_t)1);
    struct co *co2 = co_start("test2", simple_coroutine, (void*)(size_t)2);
    
    printf("Created two coroutines\n");
    
    // 让主协程也让出一次CPU
    co_yield();
    
    // 等待协程完成
    co_wait(co1);
    co_wait(co2);
    
    printf("Both coroutines completed\n");
}

void counting_coroutine(void *arg) {
    int *counter = (int*)arg;
    
    for (int i = 0; i < 5; i++) {
        (*counter)++;
        printf("Counter: %d\n", *counter);
        co_yield();
    }
}

void test_shared_data() {
    printf("\n=== Test Shared Data ===\n");
    
    int shared_counter = 0;
    
    struct co *co1 = co_start("counter1", counting_coroutine, &shared_counter);
    struct co *co2 = co_start("counter2", counting_coroutine, &shared_counter);
    
    co_yield();
    
    co_wait(co1);
    co_wait(co2);
    
    printf("Final counter value: %d\n", shared_counter);
    assert(shared_counter == 10);
    printf("Shared data test passed!\n");
}

void recursive_coroutine(void *arg) {
    int depth = (int)(size_t)arg;
    
    if (depth <= 0) {
        printf("Reached recursion base case\n");
        return;
    }
    
    printf("Recursion depth: %d\n", depth);
    
    // 创建子协程
    struct co *child = co_start("recursive_child", recursive_coroutine, 
                               (void*)(size_t)(depth - 1));
    co_wait(child);
    
    printf("Returning from depth: %d\n", depth);
}

void test_recursive_coroutines() {
    printf("\n=== Test Recursive Coroutines ===\n");
    
    struct co *root = co_start("recursive_root", recursive_coroutine, 
                              (void*)(size_t)3);
    
    co_wait(root);
    
    printf("Recursive coroutines test completed\n");
}

int main() {
    printf("Starting basic coroutine tests\n");
    
    test_basic_yield();
    // test_shared_data();
    test_recursive_coroutines();
    
    printf("\n=== All tests completed successfully! ===\n");
    return 0;
}

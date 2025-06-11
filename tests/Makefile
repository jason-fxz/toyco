# Tests Makefile for toyco library
CC = gcc
CFLAGS = -std=gnu11 -g -Wall -Wextra
LDFLAGS = -L.. -lco -lpthread
DEBUG_LDFLAGS = -L.. -lco-debug -lpthread


# 源码目录
SRC_DIR = ..
INC_DIR = ..

# 测试源文件
TEST_SOURCES = $(wildcard *.c)
TEST_TARGETS = $(TEST_SOURCES:.c=)
DEBUG_TARGETS = $(TEST_TARGETS:%=debug-%)

# 默认目标
all: $(TEST_TARGETS) $(DEBUG_TARGETS) build

# 通用规则：从.c文件生成可执行文件
%: %.c build
	$(CC) $(CFLAGS) -I$(INC_DIR) -o $@ $< $(LDFLAGS)
	$(CC) $(CFLAGS) -DDEBUG -O0 -I$(INC_DIR) -o debug-$@ $< $(DEBUG_LDFLAGS)

# 运行所有测试
run-all: $(TEST_TARGETS)
	@echo "Running all tests..."
	@for test in $(TEST_TARGETS); do \
		echo "=== Running $$test ==="; \
		LD_LIBRARY_PATH=$(SRC_DIR) ./$$test; \
		echo ""; \
	done

# 运行特定测试
run-%: %
	@echo "Running $<..."
	LD_LIBRARY_PATH=$(SRC_DIR) ./$<

build:
	@echo "Building toyco library..."
	make -C $(SRC_DIR)

# 清理
clean:
	rm -f $(TEST_TARGETS) $(DEBUG_TARGETS) *.o

.PHONY: all run-all debug clean build

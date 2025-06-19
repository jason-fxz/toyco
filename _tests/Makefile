CC = clang
CFLAGS = -std=gnu11 -Wall -Wextra -O2 -lpthread

# 测试源文件
TEST_SOURCES = $(wildcard *.c)
TEST_TARGETS = $(TEST_SOURCES:.c=)

# 默认目标
all: $(TEST_TARGETS)

# 通用规则：从.c文件生成可执行文件
%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# 清理
clean:
	rm -f $(TEST_TARGETS)

.PHONY: all clean 

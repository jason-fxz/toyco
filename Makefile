NAME := libco
CFLAGS += -U_FORTIFY_SOURCE -g -pthread -std=gnu11 -Wall -Wextra
SRCS := co.c
DEPS := $(SRCS) co.h internal.h list.h panic.h
LDFLAGS += -pthread
CC := clang

all: $(NAME).so  test-gmp

# 64bit shared library
$(NAME).so: $(DEPS)
	$(CC) -fPIC -shared -m64 $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)


# G-P-M test program
test-gmp: test_gmp.c $(DEPS)
	$(CC) -m64 $(CFLAGS) test_gmp.c $(SRCS) -o $@ $(LDFLAGS)

# Debug version with debug output enabled
debug: test_gmp.c $(DEPS)
	$(CC) -DDEBUG -m64 $(CFLAGS) test_gmp.c $(SRCS) -o test-gmp-debug $(LDFLAGS)

# Run the test
test: test-gmp
	./test-gmp

# Run with debug output
test-debug: debug
	./test-gmp-debug

clean:
	rm -f $(NAME)-*.so test-gmp test-gmp-debug

.PHONY: all test test-debug debug valgrind clean
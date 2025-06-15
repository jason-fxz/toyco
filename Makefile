NAME := libco
CFLAGS += -U_FORTIFY_SOURCE -g3 -pthread -std=gnu11 -Wall -Wextra -O0 -fno-omit-frame-pointer -fstack-protector
DEBUG_CFLAGS := -DDEBUG -O0
SRCS := co.c
DEPS := $(SRCS) co.h internal.h list.h panic.h
LDFLAGS += -pthread
CC := clang

all: $(NAME).so $(NAME)-debug.so

# 64bit shared library
$(NAME).so: $(DEPS)
	$(CC) -fPIC -shared -m64 $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

$(NAME).o: $(SRCS)
	$(CC) -fPIC -c -m64 $(CFLAGS) $(SRCS) -o $@

# debug mode
$(NAME)-debug.so: $(DEPS)
	$(CC) -fPIC -shared -m64 $(CFLAGS) $(DEBUG_CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

clean:
	rm -f $(NAME).so $(NAME)-debug.so *.o test_gmp

test_gmp: co.c test_gmp.c internal.h
	$(CC) $(CFLAGS) -I. -DDEBUG -o test_gmp co.c test_gmp.c $(LDFLAGS)

.PHONY: all clean
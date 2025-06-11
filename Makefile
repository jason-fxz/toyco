NAME := libco
CFLAGS += -U_FORTIFY_SOURCE -g -pthread -std=gnu11 -Wall -Wextra
DEBUG_CFLAGS := -DDEBUG -O0
SRCS := co.c
DEPS := $(SRCS) co.h internal.h list.h panic.h
LDFLAGS += -pthread
CC := clang

all: $(NAME).so $(NAME)-debug.so

# 64bit shared library
$(NAME).so: $(DEPS)
	$(CC) -fPIC -shared -m64 $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

# debug mode
$(NAME)-debug.so: $(DEPS)
	$(CC) -fPIC -shared -m64 $(CFLAGS) $(DEBUG_CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

clean:
	rm -f $(NAME).so $(NAME)-debug.so *.o

.PHONY: all clean
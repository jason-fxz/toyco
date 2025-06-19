MODE ?= test
NAME := libco
CFLAGS += -U_FORTIFY_SOURCE -std=gnu11 -Wall -Wextra
TEST_CFLAGS := -O2 -g3
RELEASE_CFLAGS := -O2 -g3 -DNOASSERT
DEBUG_CFLAGS := -DDEBUG -O0

# mode 
ifeq ($(MODE),release)
CFLAGS += $(RELEASE_CFLAGS)
else ifeq ($(MODE),test)
CFLAGS += $(TEST_CFLAGS)
else ifeq ($(MODE),debug)
CFLAGS += $(DEBUG_CFLAGS)
else
$(error Invalid mode: $(MODE). Use 'release', 'test', or 'debug'.)
endif
$(info Using mode: $(MODE))

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
	rm -f $(NAME).so $(NAME)-debug.so

.PHONY: all clean
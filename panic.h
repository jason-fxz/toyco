#ifndef _PANIC_H_
#define _PANIC_H_

#ifdef DEBUG
#define debug(fmt, ...) fprintf(stderr, "\033[90m[debug] " fmt "\033[0m", ##__VA_ARGS__)
#else
#define debug(fmt, ...)
#endif

#define panic(fmt, ...) do { \
    fprintf(stderr, "\033[31mPANIC\033[0m at %s:%d in %s: " fmt, __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    exit(1); \
} while (0)

#define assert(cond) do { \
    if (!(cond)) { \
        panic("assertion failed: %s\n", #cond); \
    } \
} while (0)

#endif

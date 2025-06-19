#ifndef UTILS_H
#define UTILS_H

#include <time.h>

static __thread unsigned int seed = 123456789;

unsigned int fast_rand() {
    seed = seed * 1103515245 + 12345;
    return seed;
}

double get_elapsed_time(const struct timespec *start, const struct timespec *end) {
    long sec = end->tv_sec - start->tv_sec;
    long nsec = end->tv_nsec - start->tv_nsec;
    double elapsed = sec + nsec / 1e9;
    return elapsed;
}


#endif // UTILS_H
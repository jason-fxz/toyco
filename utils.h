#ifndef UTILS_H
#define UTILS_H

static __thread unsigned int seed = 123456789;

unsigned int fast_rand() {
    seed = seed * 1103515245 + 12345;
    return seed;
}


#endif // UTILS_H
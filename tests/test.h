#ifndef TEST_H
#define TEST_H
#include <stdio.h>

static int t_fail, t_count;

#define ASSERT_EQ(got, want) do {                                          \
    long long g_ = (long long)(got), w_ = (long long)(want); t_count++;    \
    if (g_ != w_) { t_fail++;                                              \
        printf("FAIL %s:%d: %s == %lld (0x%llx), want %lld (0x%llx)\n",    \
               __FILE__, __LINE__, #got, g_, g_, w_, w_); }                \
} while (0)

#define ASSERT_TRUE(x) ASSERT_EQ(!!(x), 1)

#define TEST_MAIN_END() do {                                               \
    printf("%-24s %3d assertions, %d failures\n", __FILE__, t_count, t_fail); \
    return t_fail ? 1 : 0;                                                 \
} while (0)

#endif

// Minimal host test harness: no dependencies. Each test binary calls its test
// functions from main() and returns check_result().
#pragma once
#include <cstdio>

inline int& check_failures()
{
    static int n = 0;
    return n;
}

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++check_failures();                                                       \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                \
    do {                                                                              \
        long long va_ = static_cast<long long>(a), vb_ = static_cast<long long>(b);   \
        if (va_ != vb_) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", \
                         __FILE__, __LINE__, #a, #b, va_, vb_);                       \
            ++check_failures();                                                       \
        }                                                                             \
    } while (0)

#define RUN(test)                                    \
    do {                                             \
        int before_ = check_failures();              \
        test();                                      \
        std::printf("%s %s\n", check_failures() == before_ ? "PASS" : "FAIL", #test); \
    } while (0)

inline int check_result()
{
    if (check_failures()) std::fprintf(stderr, "%d check(s) failed\n", check_failures());
    return check_failures() ? 1 : 0;
}

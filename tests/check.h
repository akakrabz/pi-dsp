// A tiny test harness: TEST(name) { CHECK(cond); CHECK_NEAR(a, b, eps); }
#pragma once
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check {
struct Test {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Test>& registry() {
    static std::vector<Test> t;
    return t;
}
inline int& failures() {
    static int f = 0;
    return f;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
}  // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name)                                                        \
    static void name();                                                   \
    static check::Reg CHECK_CAT(reg_, name)(#name, name);                 \
    static void name()

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            check::failures()++;                                                          \
        }                                                                                 \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                              \
    do {                                                                                                   \
        double _a = (a), _b = (b);                                                                         \
        if (!(std::fabs(_a - _b) <= (eps))) {                                                              \
            std::printf("    FAIL %s:%d: %s = %g, expected %g +- %g\n", __FILE__, __LINE__, #a, _a, _b, (double)(eps)); \
            check::failures()++;                                                                           \
        }                                                                                                  \
    } while (0)

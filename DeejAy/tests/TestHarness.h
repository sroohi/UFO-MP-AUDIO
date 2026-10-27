// Tiny dependency-free test harness (shared with MelodyPymp II).
#pragma once
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mptest
{
struct Case { std::string name; std::function<void()> fn; };
inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline int& failures() { static int f = 0; return f; }
struct Registrar { Registrar (const char* n, std::function<void()> f) { cases().push_back ({ n, std::move (f) }); } };

inline void check (bool ok, const char* expr, const char* file, int line)
{
    if (! ok) { ++failures(); std::printf ("    FAIL %s:%d: %s\n", file, line, expr); }
}
inline void checkNear (double a, double b, double tol, const char* ea, const char* eb, const char* file, int line)
{
    if (std::abs (a - b) > tol) { ++failures(); std::printf ("    FAIL %s:%d: %s (%.9g) != %s (%.9g) within %g\n", file, line, ea, a, eb, b, tol); }
}
inline int runAll()
{
    int failedCases = 0;
    for (auto& c : cases())
    {
        const int before = failures();
        std::printf ("[ RUN  ] %s\n", c.name.c_str());
        c.fn();
        if (failures() != before) { ++failedCases; std::printf ("[ FAIL ] %s\n", c.name.c_str()); }
        else std::printf ("[  OK  ] %s\n", c.name.c_str());
    }
    std::printf ("%d cases, %d failed, %d assertions failed\n", (int) cases().size(), failedCases, failures());
    return failedCases == 0 ? 0 : 1;
}
}

#define MP_TEST(name) static void name##_body(); static mptest::Registrar name##_reg (#name, name##_body); static void name##_body()
#define CHECK(expr) mptest::check ((expr), #expr, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) mptest::checkNear ((double) (a), (double) (b), (tol), #a, #b, __FILE__, __LINE__)
#define REQUIRE(expr) do { if (! (expr)) { mptest::check (false, #expr, __FILE__, __LINE__); return; } } while (0)

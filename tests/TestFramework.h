// Flubsound Pro - tiny self-registering test framework (no dependencies).
//
//   TEST_CASE ("ParametricEq: bell gain at centre") { CHECK_NEAR (x, 6.0, 0.05); }
//
// Run: flub_tests [substring-filter]. Exit code = number of failed cases.
// AllocationGuard counts heap allocations on the current thread (global
// operator new is replaced in TestMain.cpp) so tests can assert that
// process() paths are allocation-free.
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace flubtest
{
struct TestCase
{
    std::string name;
    std::function<void()> fn;
    const char* file;
    int line;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> r;
    return r;
}

struct Registrar
{
    Registrar (const char* name, std::function<void()> fn, const char* file, int line)
    {
        registry().push_back ({ name, std::move (fn), file, line });
    }
};

struct Failure
{
};

inline int& currentFailures()
{
    static int f = 0;
    return f;
}

inline void reportFailure (const char* file, int line, const std::string& msg)
{
    ++currentFailures();
    std::cerr << "    FAILED " << file << ":" << line << "  " << msg << "\n";
}

// Allocation tracking (implemented in TestMain.cpp)
int64_t allocationCount() noexcept;

class AllocationGuard
{
public:
    AllocationGuard() noexcept : start (allocationCount()) {}
    int64_t allocations() const noexcept { return allocationCount() - start; }

private:
    int64_t start;
};
} // namespace flubtest

#define FLUB_TEST_CONCAT2(a, b) a##b
#define FLUB_TEST_CONCAT(a, b) FLUB_TEST_CONCAT2 (a, b)

#define TEST_CASE(name)                                                                                     \
    static void FLUB_TEST_CONCAT (flubTestFn_, __LINE__)();                                                 \
    static ::flubtest::Registrar FLUB_TEST_CONCAT (flubTestReg_, __LINE__) (name, &FLUB_TEST_CONCAT (flubTestFn_, __LINE__), __FILE__, __LINE__); \
    static void FLUB_TEST_CONCAT (flubTestFn_, __LINE__)()

#define CHECK(cond)                                                                 \
    do                                                                              \
    {                                                                               \
        if (! (cond))                                                               \
            ::flubtest::reportFailure (__FILE__, __LINE__, "CHECK(" #cond ")");     \
    } while (0)

#define REQUIRE(cond)                                                               \
    do                                                                              \
    {                                                                               \
        if (! (cond))                                                               \
        {                                                                           \
            ::flubtest::reportFailure (__FILE__, __LINE__, "REQUIRE(" #cond ")");   \
            throw ::flubtest::Failure {};                                           \
        }                                                                           \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                                                        \
    do                                                                                                 \
    {                                                                                                  \
        const double flubA_ = static_cast<double> (actual);                                            \
        const double flubE_ = static_cast<double> (expected);                                          \
        const double flubT_ = static_cast<double> (tolerance);                                         \
        if (! (std::abs (flubA_ - flubE_) <= flubT_))                                                  \
        {                                                                                              \
            std::ostringstream flubS_;                                                                 \
            flubS_ << "CHECK_NEAR(" #actual ", " #expected ", " #tolerance ") actual=" << flubA_         \
                   << " expected=" << flubE_ << " tol=" << flubT_;                                    \
            ::flubtest::reportFailure (__FILE__, __LINE__, flubS_.str());                              \
        }                                                                                              \
    } while (0)

#define CHECK_LE(a, b)                                                                                  \
    do                                                                                                 \
    {                                                                                                  \
        const double flubA_ = static_cast<double> (a);                                                 \
        const double flubB_ = static_cast<double> (b);                                                 \
        if (! (flubA_ <= flubB_))                                                                      \
        {                                                                                              \
            std::ostringstream flubS_;                                                                 \
            flubS_ << "CHECK_LE(" #a ", " #b ") " << flubA_ << " > " << flubB_;                         \
            ::flubtest::reportFailure (__FILE__, __LINE__, flubS_.str());                              \
        }                                                                                              \
    } while (0)

#define CHECK_GE(a, b) CHECK_LE (b, a)

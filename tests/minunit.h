#pragma once
#include <cstdio>
#include <cstdlib>
#include <cmath>

extern int tests_run;
extern int tests_failed;
extern int assertions_passed;
extern int assertions_failed;

#define MU_ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        assertions_failed++; \
        fprintf(stderr, "  FAIL %s:%d: %s: " msg "\n", __FILE__, __LINE__, __FUNCTION__); \
    } else { \
        assertions_passed++; \
    } \
} while(0)

#define MU_CHECK(cond) MU_ASSERT(cond, #cond)

#define MU_CHECK_EQ(a, b) MU_ASSERT((a) == (b), "expected " #a " == " #b)
#define MU_CHECK_NE(a, b) MU_ASSERT((a) != (b), "expected " #a " != " #b)
#define MU_CHECK_DBL(a, b, eps) MU_ASSERT(fabs((a) - (b)) < (eps), "expected " #a " ≈ " #b)

#define MU_RUN_TEST(test) do { \
    int _fail_before = assertions_failed; \
    fprintf(stderr, "  %s ... ", #test); \
    test(); \
    fprintf(stderr, "%s\n", assertions_failed > _fail_before ? "FAIL" : "OK"); \
} while(0)

#define MU_RUN_SUITE(suite) do { \
    fprintf(stderr, "\n=== %s ===\n", #suite); \
    suite(); \
} while(0)

#define MU_REPORT() do { \
    fprintf(stderr, "\n=== RESULTS ===\n"); \
    fprintf(stderr, "  Assertions: %d passed, %d failed\n", assertions_passed, assertions_failed); \
    fprintf(stderr, "  Tests:      %d run, %d failed\n", tests_run, assertions_failed); \
    if (assertions_failed > 0) exit(1); \
} while(0)

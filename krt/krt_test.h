/* Minimal TAP-ish harness for the krt C tests. */
#ifndef PERL_KRT_TEST_H
#define PERL_KRT_TEST_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int krt_t_n = 0, krt_t_fail = 0;

#define ok(cond, ...) do { \
    krt_t_n++; \
    if (cond) printf("ok %d - ", krt_t_n); \
    else { printf("not ok %d - ", krt_t_n); krt_t_fail++; } \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); \
} while (0)

#define is_int(got, want, ...) do { \
    long long _g = (long long)(got), _w = (long long)(want); \
    krt_t_n++; \
    if (_g == _w) printf("ok %d - ", krt_t_n); \
    else { printf("not ok %d - ", krt_t_n); krt_t_fail++; } \
    printf(__VA_ARGS__); \
    if (_g != _w) printf(" (got %lld, want %lld)", _g, _w); \
    printf("\n"); fflush(stdout); \
} while (0)

#define skip_all(...) do { printf("1..0 # SKIP "); printf(__VA_ARGS__); \
    printf("\n"); return 0; } while (0)

#define done_testing() do { printf("1..%d\n", krt_t_n); \
    return krt_t_fail ? 1 : 0; } while (0)

#endif

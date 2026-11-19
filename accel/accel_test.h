/* Minimal TAP harness for the accel C unit tests. */
#ifndef PERL_ACCEL_TEST_H
#define PERL_ACCEL_TEST_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int accel_t_n = 0, accel_t_fail = 0;

#define ok(cond, ...) do { \
    accel_t_n++; \
    if (cond) printf("ok %d - ", accel_t_n); \
    else { printf("not ok %d - ", accel_t_n); accel_t_fail++; } \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); \
} while (0)

#define is_int(got, want, ...) do { \
    long long _g = (long long)(got), _w = (long long)(want); \
    accel_t_n++; \
    if (_g == _w) printf("ok %d - ", accel_t_n); \
    else { printf("not ok %d - ", accel_t_n); accel_t_fail++; } \
    printf(__VA_ARGS__); \
    if (_g != _w) printf(" (got %lld, want %lld)", _g, _w); \
    printf("\n"); fflush(stdout); \
} while (0)

#define is_str(got, want, ...) do { \
    const char *_g = (got), *_w = (want); \
    int _eq = (_g && _w && strcmp(_g, _w) == 0); \
    accel_t_n++; \
    if (_eq) printf("ok %d - ", accel_t_n); \
    else { printf("not ok %d - ", accel_t_n); accel_t_fail++; } \
    printf(__VA_ARGS__); \
    if (!_eq) printf(" (got '%s', want '%s')", _g ? _g : "(null)", _w ? _w : "(null)"); \
    printf("\n"); fflush(stdout); \
} while (0)

static inline int
accel_f64_bit_eq(double a, double b)
{
    uint64_t ua, ub;
    memcpy(&ua, &a, sizeof(ua));
    memcpy(&ub, &b, sizeof(ub));
    /* Canonical NaN match: any NaN matches NaN */
    if (a != a && b != b) {
        return 1;
    }
    return ua == ub;
}

#define is_f64_bits(got, want, ...) do { \
    double _g = (got), _w = (want); \
    uint64_t _ug, _uw; \
    memcpy(&_ug, &_g, sizeof(_ug)); \
    memcpy(&_uw, &_w, sizeof(_uw)); \
    accel_t_n++; \
    if (accel_f64_bit_eq(_g, _w)) printf("ok %d - ", accel_t_n); \
    else { printf("not ok %d - ", accel_t_n); accel_t_fail++; } \
    printf(__VA_ARGS__); \
    if (!accel_f64_bit_eq(_g, _w)) \
        printf(" (got %.17g [0x%016llx], want %.17g [0x%016llx])", \
               _g, (unsigned long long)_ug, _w, (unsigned long long)_uw); \
    printf("\n"); fflush(stdout); \
} while (0)

#define done_testing() do { \
    printf("1..%d\n", accel_t_n); \
    return accel_t_fail ? 1 : 0; \
} while (0)

#endif /* PERL_ACCEL_TEST_H */

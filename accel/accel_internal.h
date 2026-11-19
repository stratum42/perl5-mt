/*
 * Internal declarations for the perl5-mt accel library.
 */

#ifndef PERL_ACCEL_INTERNAL_H
#define PERL_ACCEL_INTERNAL_H

#include "perl_accel.h"

/* Built-in backend descriptors */
extern const perl_accel_backend_t perl_accel_backend_scalar;
extern const perl_accel_backend_t perl_accel_backend_accelerate;
extern const perl_accel_backend_t perl_accel_backend_metal;
extern const perl_accel_backend_t perl_accel_backend_ane;
extern const perl_accel_backend_t perl_accel_backend_mesa_opencl;
extern const perl_accel_backend_t perl_accel_backend_intel_npu;
extern const perl_accel_backend_t perl_accel_backend_amd_npu;
extern const perl_accel_backend_t perl_accel_backend_power_nx;
extern const perl_accel_backend_t perl_accel_backend_s390x_nnpa;

/*
 * Exact Shewchuk adaptive floating-point expansion accumulator.
 * Shared between accel_scalar.c and accelerated CR backends (e.g. Ozaki
 * slice accumulation in accel_darwin_accelerate.c) to guarantee bit-identical
 * IEEE-754 round-to-nearest-even results.
 */
#define PERL_ACCEL_EXP_INLINE_CAP 64

typedef struct perl_accel_exp {
    double  inline_buf[PERL_ACCEL_EXP_INLINE_CAP];
    double *e;
    size_t  len;
    size_t  cap;
    int     oom;
} perl_accel_exp_t;

void   perl_accel_exp_init(perl_accel_exp_t *acc);
void   perl_accel_exp_free(perl_accel_exp_t *acc);
void   perl_accel_exp_add(perl_accel_exp_t *acc, double x);
void   perl_accel_exp_add_prod(perl_accel_exp_t *acc, double a, double b);
double perl_accel_exp_round_rne(perl_accel_exp_t *acc);

#endif /* PERL_ACCEL_INTERNAL_H */

/*
 * perl5-mt Extensible Native Acceleration & Transparent Offload Framework.
 *
 * Provides a backend vtable/registry (perl_accel_backend_t), runtime
 * capability discovery, threshold-based transparent offload dispatch,
 * environment control (PERL_ACCEL=auto|off|<backend>, PERL_ACCEL_THRESHOLD),
 * and mandatory scalar-twin / correctly-rounded (CR) fallback.
 *
 * Reductions sum_f64_cr and dot_f64_cr are defined as the IEEE-754 binary64
 * round-to-nearest-even value of the exact mathematical sum / dot product.
 * Every accelerated backend MUST agree bit-for-bit with the scalar twin.
 */

#ifndef PERL_ACCEL_H
#define PERL_ACCEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum perl_accel_id {
    PERL_ACCEL_SCALAR       = 0,
    PERL_ACCEL_ACCELERATE   = 1, /* macOS Accelerate (vDSP / BLAS) */
    PERL_ACCEL_METAL        = 2, /* macOS Apple Metal GPU */
    PERL_ACCEL_ANE          = 3, /* Apple Silicon Neural Engine */
    PERL_ACCEL_MESA_OPENCL  = 4, /* Mesa OpenCL (AMD & Intel GPUs only) */
    PERL_ACCEL_INTEL_NPU    = 5, /* Intel NPU / Level Zero */
    PERL_ACCEL_AMD_NPU      = 6, /* AMD XDNA / XRT NPU */
    PERL_ACCEL_POWER_NX     = 7, /* IBM POWER NX coprocessor */
    PERL_ACCEL_S390X_NNPA   = 8, /* IBM z16 / Telum II NNPA */
    PERL_ACCEL_BACKEND_MAX
} perl_accel_id_t;

typedef enum perl_accel_device_class {
    PERL_ACCEL_DEV_CPU    = 0,
    PERL_ACCEL_DEV_GPU    = 1,
    PERL_ACCEL_DEV_NPU    = 2,
    PERL_ACCEL_DEV_COPROC = 3
} perl_accel_device_class_t;

/* Capability bitmask for perl_accel_backend_t.caps */
#define PERL_ACCEL_CAP_SUM_F64_CR   (1u << 0)
#define PERL_ACCEL_CAP_DOT_F64_CR   (1u << 1)
#define PERL_ACCEL_CAP_VEC_ADD_F64  (1u << 2)
#define PERL_ACCEL_CAP_VEC_MUL_F64  (1u << 3)
#define PERL_ACCEL_CAP_VEC_AXPY_F64 (1u << 4)
#define PERL_ACCEL_CAP_DOT_I8_I32   (1u << 5)
#define PERL_ACCEL_CAP_COMPRESS_NX  (1u << 6)

/* Default transparent offload thresholds (elements / bytes) */
#define PERL_ACCEL_DEFAULT_THRESHOLD_F64  4096u
#define PERL_ACCEL_DEFAULT_THRESHOLD_I8   1024u
#define PERL_ACCEL_DEFAULT_THRESHOLD_NX   4096u

typedef struct perl_accel_backend {
    perl_accel_id_t           id;
    const char               *name;
    const char               *description;
    perl_accel_device_class_t device_class;
    int                       priority;    /* higher wins in PERL_ACCEL=auto */
    uint32_t                  caps;        /* PERL_ACCEL_CAP_* bitmask */
    size_t                    min_len_f64; /* threshold for f64 ops */
    size_t                    min_len_i8;  /* threshold for i8 ops */
    size_t                    min_len_nx;  /* threshold for nx compression */

    /* Lifecycle / runtime hardware probe */
    int  (*probe)(void); /* returns 1 if usable on this host, 0 otherwise */
    int  (*init)(void);  /* returns 0 on success, <0 on failure */
    void (*shutdown)(void);

    /* Core operations: return 0 on success, <0 to fall back to scalar twin */
    int (*sum_f64_cr)(const double *a, size_t n, double *out);
    int (*dot_f64_cr)(const double *a, const double *b, size_t n, double *out);
    int (*vec_add_f64)(const double *a, const double *b, double *out, size_t n);
    int (*vec_mul_f64)(const double *a, const double *b, double *out, size_t n);
    int (*vec_axpy_f64)(double alpha, const double *x, const double *y,
                        double *out, size_t n);
    int (*dot_i8_i32)(const int8_t *a, const int8_t *b, size_t n, int32_t *out);
    int (*compress_nx)(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t *dst_len);
} perl_accel_backend_t;

typedef struct perl_accel_stats {
    uint64_t    offload_calls;
    uint64_t    fallback_calls;
    const char *last_backend_name;
} perl_accel_stats_t;

/* Registry & lifecycle */
int                         perl_accel_init(void);
void                        perl_accel_shutdown(void);
int                         perl_accel_register(const perl_accel_backend_t *backend);
const perl_accel_backend_t *perl_accel_active(void);
const perl_accel_backend_t *perl_accel_find(const char *name);
size_t                      perl_accel_list(const perl_accel_backend_t **out, size_t max_out);
int                         perl_accel_backend_available(const char *name);
int                         perl_accel_is_enabled(void);

/* Telemetry & test control */
void perl_accel_get_stats(perl_accel_stats_t *out);
void perl_accel_reset_stats(void);
void perl_accel_reset_for_test(const char *mode_override, size_t threshold_override);

/* Transparent offload dispatch API (always succeeds via scalar twin fallback) */
double  perl_accel_sum_f64_cr(const double *a, size_t n);
double  perl_accel_dot_f64_cr(const double *a, const double *b, size_t n);
void    perl_accel_vec_add_f64(const double *a, const double *b, double *out, size_t n);
void    perl_accel_vec_mul_f64(const double *a, const double *b, double *out, size_t n);
void    perl_accel_vec_axpy_f64(double alpha, const double *x, const double *y,
                                double *out, size_t n);
int32_t perl_accel_dot_i8_i32(const int8_t *a, const int8_t *b, size_t n);
int     perl_accel_compress_nx(const uint8_t *src, size_t src_len,
                               uint8_t *dst, size_t *dst_len);

/* Direct reference scalar twin (always exact, used for fallback & verification) */
double  perl_accel_scalar_sum_f64_cr(const double *a, size_t n);
double  perl_accel_scalar_dot_f64_cr(const double *a, const double *b, size_t n);
void    perl_accel_scalar_vec_add_f64(const double *a, const double *b, double *out, size_t n);
void    perl_accel_scalar_vec_mul_f64(const double *a, const double *b, double *out, size_t n);
void    perl_accel_scalar_vec_axpy_f64(double alpha, const double *x, const double *y,
                                       double *out, size_t n);
int32_t perl_accel_scalar_dot_i8_i32(const int8_t *a, const int8_t *b, size_t n);

/*
 * Mesa OpenCL vendor policy filter (AMD & Intel GPUs ONLY; unconditionally
 * rejects NVIDIA / CUDA). Exported for direct unit testing on all hosts.
 */
int perl_accel_mesa_opencl_vendor_allowed(uint32_t vendor_id,
                                          const char *vendor_name,
                                          const char *platform_name);

#ifdef __cplusplus
}
#endif

#endif /* PERL_ACCEL_H */

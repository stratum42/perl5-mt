/*
 * perl5-mt Extensible Native Acceleration & Transparent Offload Registry.
 *
 * Manages backend registration, runtime capability discovery, environment
 * selection (PERL_ACCEL=auto|off|<backend>, PERL_ACCEL_THRESHOLD=<n>),
 * threshold-based transparent dispatch, and mandatory scalar-twin fallback.
 */

#include "accel_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PERL_ACCEL_REGISTRY_CAP 32

typedef struct accel_entry {
    const perl_accel_backend_t *backend;
    int                         available;
    int                         initialized;
} accel_entry_t;

static accel_entry_t               S_registry[PERL_ACCEL_REGISTRY_CAP];
static size_t                      S_registry_len = 0;
static int                         S_inited = 0;
static int                         S_enabled = 1;
static size_t                      S_threshold_override = 0;
static int                         S_has_threshold_override = 0;
static const perl_accel_backend_t *S_active = NULL;
static perl_accel_stats_t          S_stats = { 0, 0, NULL };

static void
S_register_builtins(void)
{
    static const perl_accel_backend_t * const builtins[] = {
        &perl_accel_backend_scalar,
        &perl_accel_backend_accelerate,
        &perl_accel_backend_metal,
        &perl_accel_backend_ane,
        &perl_accel_backend_mesa_opencl,
        &perl_accel_backend_intel_npu,
        &perl_accel_backend_amd_npu,
        &perl_accel_backend_power_nx,
        &perl_accel_backend_s390x_nnpa
    };
    size_t i;

    for (i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        (void)perl_accel_register(builtins[i]);
    }
}

int
perl_accel_register(const perl_accel_backend_t *backend)
{
    size_t i;

    if (!backend || !backend->name) {
        return -EINVAL;
    }
    for (i = 0; i < S_registry_len; i++) {
        if (strcmp(S_registry[i].backend->name, backend->name) == 0) {
            return 0; /* already registered */
        }
    }
    if (S_registry_len >= PERL_ACCEL_REGISTRY_CAP) {
        return -ENOMEM;
    }

    S_registry[S_registry_len].backend = backend;
    S_registry[S_registry_len].available = backend->probe ? (backend->probe() ? 1 : 0) : 0;
    S_registry[S_registry_len].initialized = 0;
    S_registry_len++;
    return 0;
}

static accel_entry_t *
S_find_entry(const char *name)
{
    size_t i;
    if (!name) {
        return NULL;
    }
    for (i = 0; i < S_registry_len; i++) {
        if (strcmp(S_registry[i].backend->name, name) == 0) {
            return &S_registry[i];
        }
    }
    return NULL;
}

static int
S_activate_entry(accel_entry_t *ent)
{
    if (!ent || !ent->available) {
        return -1;
    }
    if (!ent->initialized && ent->backend->init) {
        if (ent->backend->init() != 0) {
            ent->available = 0;
            return -1;
        }
        ent->initialized = 1;
    }
    S_active = ent->backend;
    return 0;
}

static void
S_init_with_mode(const char *mode, int has_thresh, size_t thresh)
{
    const char *env_force;
    const char *env_thresh;
    size_t i;

    S_register_builtins();

    S_enabled = 1;
    S_active = &perl_accel_backend_scalar;
    S_has_threshold_override = has_thresh;
    S_threshold_override = thresh;

    if (!has_thresh) {
        env_thresh = getenv("PERL_ACCEL_THRESHOLD");
        if (env_thresh && *env_thresh) {
            char *endp = NULL;
            unsigned long v = strtoul(env_thresh, &endp, 10);
            if (endp && endp != env_thresh && *endp == '\0') {
                S_has_threshold_override = 1;
                S_threshold_override = (size_t)v;
            }
        }
    }

    /* PERL_FORCE_SCALAR=1 also forces scalar-only mode unless overridden */
    env_force = getenv("PERL_FORCE_SCALAR");
    if (!mode && env_force && *env_force && strcmp(env_force, "0") != 0) {
        S_enabled = 0;
        S_active = &perl_accel_backend_scalar;
        S_inited = 1;
        return;
    }

    if (!mode) {
        mode = getenv("PERL_ACCEL");
    }
    if (!mode || !*mode || strcmp(mode, "auto") == 0) {
        /* Pick highest-priority available backend */
        accel_entry_t *best = NULL;
        for (i = 0; i < S_registry_len; i++) {
            if (!S_registry[i].available) {
                continue;
            }
            if (!best || S_registry[i].backend->priority > best->backend->priority) {
                best = &S_registry[i];
            }
        }
        if (best && S_activate_entry(best) != 0) {
            S_active = &perl_accel_backend_scalar;
        }
    } else if (strcmp(mode, "off") == 0 || strcmp(mode, "0") == 0 ||
               strcmp(mode, "none") == 0) {
        S_enabled = 0;
        S_active = &perl_accel_backend_scalar;
    } else if (strcmp(mode, "scalar") == 0) {
        S_enabled = 1;
        S_active = &perl_accel_backend_scalar;
    } else {
        accel_entry_t *ent = S_find_entry(mode);
        if (!ent || S_activate_entry(ent) != 0) {
            /* Requested backend unavailable: fall back cleanly to scalar */
            S_active = &perl_accel_backend_scalar;
        }
    }

    S_inited = 1;
}

int
perl_accel_init(void)
{
    if (!S_inited) {
        S_init_with_mode(NULL, 0, 0);
    }
    return 0;
}

void
perl_accel_shutdown(void)
{
    size_t i;
    for (i = 0; i < S_registry_len; i++) {
        if (S_registry[i].initialized && S_registry[i].backend->shutdown) {
            S_registry[i].backend->shutdown();
        }
        S_registry[i].initialized = 0;
    }
    S_registry_len = 0;
    S_active = NULL;
    S_inited = 0;
    S_enabled = 1;
    S_has_threshold_override = 0;
    S_threshold_override = 0;
}

void
perl_accel_reset_for_test(const char *mode_override, size_t threshold_override)
{
    perl_accel_shutdown();
    perl_accel_reset_stats();
    S_init_with_mode(mode_override,
                     threshold_override != (size_t)-1 ? 1 : 0,
                     threshold_override != (size_t)-1 ? threshold_override : 0);
}

const perl_accel_backend_t *
perl_accel_active(void)
{
    if (!S_inited) {
        (void)perl_accel_init();
    }
    return S_active ? S_active : &perl_accel_backend_scalar;
}

const perl_accel_backend_t *
perl_accel_find(const char *name)
{
    accel_entry_t *ent;
    if (!S_inited) {
        (void)perl_accel_init();
    }
    ent = S_find_entry(name);
    return ent ? ent->backend : NULL;
}

size_t
perl_accel_list(const perl_accel_backend_t **out, size_t max_out)
{
    size_t i, count;
    if (!S_inited) {
        (void)perl_accel_init();
    }
    count = (S_registry_len < max_out) ? S_registry_len : max_out;
    for (i = 0; i < count; i++) {
        out[i] = S_registry[i].backend;
    }
    return S_registry_len;
}

int
perl_accel_backend_available(const char *name)
{
    accel_entry_t *ent;
    if (!S_inited) {
        (void)perl_accel_init();
    }
    ent = S_find_entry(name);
    return ent ? ent->available : 0;
}

int
perl_accel_is_enabled(void)
{
    if (!S_inited) {
        (void)perl_accel_init();
    }
    return S_enabled;
}

void
perl_accel_get_stats(perl_accel_stats_t *out)
{
    if (out) {
        *out = S_stats;
    }
}

void
perl_accel_reset_stats(void)
{
    S_stats.offload_calls = 0;
    S_stats.fallback_calls = 0;
    S_stats.last_backend_name = NULL;
}

static size_t
S_eff_threshold(size_t backend_min)
{
    return S_has_threshold_override ? S_threshold_override : backend_min;
}

/* ---- Transparent Offload Dispatch Functions ------------------------------- */

double
perl_accel_sum_f64_cr(const double *a, size_t n)
{
    const perl_accel_backend_t *b = perl_accel_active();
    double out = 0.0;

    if (S_enabled && b->id != PERL_ACCEL_SCALAR &&
        (b->caps & PERL_ACCEL_CAP_SUM_F64_CR) && b->sum_f64_cr &&
        n >= S_eff_threshold(b->min_len_f64)) {
        if (b->sum_f64_cr(a, n, &out) == 0) {
            S_stats.offload_calls++;
            S_stats.last_backend_name = b->name;
            return out;
        }
    }

    S_stats.fallback_calls++;
    S_stats.last_backend_name = "scalar";
    return perl_accel_scalar_sum_f64_cr(a, n);
}

double
perl_accel_dot_f64_cr(const double *a, const double *b_vec, size_t n)
{
    const perl_accel_backend_t *b = perl_accel_active();
    double out = 0.0;

    if (S_enabled && b->id != PERL_ACCEL_SCALAR &&
        (b->caps & PERL_ACCEL_CAP_DOT_F64_CR) && b->dot_f64_cr &&
        n >= S_eff_threshold(b->min_len_f64)) {
        if (b->dot_f64_cr(a, b_vec, n, &out) == 0) {
            S_stats.offload_calls++;
            S_stats.last_backend_name = b->name;
            return out;
        }
    }

    S_stats.fallback_calls++;
    S_stats.last_backend_name = "scalar";
    return perl_accel_scalar_dot_f64_cr(a, b_vec, n);
}

void
perl_accel_vec_add_f64(const double *a, const double *b_vec, double *out, size_t n)
{
    const perl_accel_backend_t *b = perl_accel_active();

    if (S_enabled && b->id != PERL_ACCEL_SCALAR &&
        (b->caps & PERL_ACCEL_CAP_VEC_ADD_F64) && b->vec_add_f64 &&
        n >= S_eff_threshold(b->min_len_f64)) {
        if (b->vec_add_f64(a, b_vec, out, n) == 0) {
            S_stats.offload_calls++;
            S_stats.last_backend_name = b->name;
            return;
        }
    }

    S_stats.fallback_calls++;
    S_stats.last_backend_name = "scalar";
    perl_accel_scalar_vec_add_f64(a, b_vec, out, n);
}

void
perl_accel_vec_mul_f64(const double *a, const double *b_vec, double *out, size_t n)
{
    const perl_accel_backend_t *b = perl_accel_active();

    if (S_enabled && b->id != PERL_ACCEL_SCALAR &&
        (b->caps & PERL_ACCEL_CAP_VEC_MUL_F64) && b->vec_mul_f64 &&
        n >= S_eff_threshold(b->min_len_f64)) {
        if (b->vec_mul_f64(a, b_vec, out, n) == 0) {
            S_stats.offload_calls++;
            S_stats.last_backend_name = b->name;
            return;
        }
    }

    S_stats.fallback_calls++;
    S_stats.last_backend_name = "scalar";
    perl_accel_scalar_vec_mul_f64(a, b_vec, out, n);
}

void
perl_accel_vec_axpy_f64(double alpha, const double *x, const double *y,
                        double *out, size_t n)
{
    const perl_accel_backend_t *b = perl_accel_active();

    if (S_enabled && b->id != PERL_ACCEL_SCALAR &&
        (b->caps & PERL_ACCEL_CAP_VEC_AXPY_F64) && b->vec_axpy_f64 &&
        n >= S_eff_threshold(b->min_len_f64)) {
        if (b->vec_axpy_f64(alpha, x, y, out, n) == 0) {
            S_stats.offload_calls++;
            S_stats.last_backend_name = b->name;
            return;
        }
    }

    S_stats.fallback_calls++;
    S_stats.last_backend_name = "scalar";
    perl_accel_scalar_vec_axpy_f64(alpha, x, y, out, n);
}

int32_t
perl_accel_dot_i8_i32(const int8_t *a, const int8_t *b_vec, size_t n)
{
    const perl_accel_backend_t *b = perl_accel_active();
    int32_t out = 0;

    if (S_enabled && b->id != PERL_ACCEL_SCALAR &&
        (b->caps & PERL_ACCEL_CAP_DOT_I8_I32) && b->dot_i8_i32 &&
        n >= S_eff_threshold(b->min_len_i8)) {
        if (b->dot_i8_i32(a, b_vec, n, &out) == 0) {
            S_stats.offload_calls++;
            S_stats.last_backend_name = b->name;
            return out;
        }
    }

    S_stats.fallback_calls++;
    S_stats.last_backend_name = "scalar";
    return perl_accel_scalar_dot_i8_i32(a, b_vec, n);
}

int
perl_accel_compress_nx(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t *dst_len)
{
    const perl_accel_backend_t *b = perl_accel_active();
    size_t i;

    if (!S_enabled) {
        S_stats.fallback_calls++;
        S_stats.last_backend_name = "scalar";
        return -ENOTSUP;
    }

    if ((b->caps & PERL_ACCEL_CAP_COMPRESS_NX) && b->compress_nx &&
        src_len >= S_eff_threshold(b->min_len_nx)) {
        if (b->compress_nx(src, src_len, dst, dst_len) == 0) {
            S_stats.offload_calls++;
            S_stats.last_backend_name = b->name;
            return 0;
        }
    }

    /* Check if any available coprocessor backend offers compress_nx */
    for (i = 0; i < S_registry_len; i++) {
        const perl_accel_backend_t *cand = S_registry[i].backend;
        if (S_registry[i].available && cand != b &&
            (cand->caps & PERL_ACCEL_CAP_COMPRESS_NX) && cand->compress_nx &&
            src_len >= S_eff_threshold(cand->min_len_nx)) {
            if (cand->compress_nx(src, src_len, dst, dst_len) == 0) {
                S_stats.offload_calls++;
                S_stats.last_backend_name = cand->name;
                return 0;
            }
        }
    }

    S_stats.fallback_calls++;
    S_stats.last_backend_name = "scalar";
    return -ENOTSUP;
}

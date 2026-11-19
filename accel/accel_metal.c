/*
 * macOS Apple Metal GPU runtime-probed backend for perl5-mt accel.
 *
 * Uses pure C dynamic loading of Metal.framework (no Objective-C) to detect
 * a Metal GPU at runtime.  Dispatch hooks gracefully fall back to Accelerate
 * or scalar until Metal compute pipeline shaders are bound.
 */

#include "accel_internal.h"

#if defined(__APPLE__)
#  include <dlfcn.h>
#  include <stdlib.h>

static void *S_metal_dl = NULL;
static void *S_metal_dev = NULL;

typedef void *(*mtl_create_dev_fn)(void);
typedef void  (*cf_release_fn)(const void *);

static int
S_metal_probe(void)
{
    void *h;
    mtl_create_dev_fn create_dev;
    void *dev;

    h = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        return 0;
    }

    create_dev = (mtl_create_dev_fn)dlsym(h, "MTLCreateSystemDefaultDevice");
    if (!create_dev) {
        dlclose(h);
        return 0;
    }

    dev = create_dev();
    if (!dev) {
        dlclose(h);
        return 0;
    }

    /* Release temporary probe device via CoreFoundation CFRelease if present */
    {
        void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
                          RTLD_LAZY | RTLD_LOCAL);
        if (cf) {
            cf_release_fn rel = (cf_release_fn)dlsym(cf, "CFRelease");
            if (rel) {
                rel(dev);
            }
            dlclose(cf);
        }
    }
    dlclose(h);
    return 1;
}

static int
S_metal_init(void)
{
    mtl_create_dev_fn create_dev;

    if (S_metal_dev) {
        return 0;
    }
    S_metal_dl = dlopen("/System/Library/Frameworks/Metal.framework/Metal",
                        RTLD_LAZY | RTLD_LOCAL);
    if (!S_metal_dl) {
        return -1;
    }
    create_dev = (mtl_create_dev_fn)dlsym(S_metal_dl, "MTLCreateSystemDefaultDevice");
    if (!create_dev) {
        dlclose(S_metal_dl);
        S_metal_dl = NULL;
        return -1;
    }
    S_metal_dev = create_dev();
    if (!S_metal_dev) {
        dlclose(S_metal_dl);
        S_metal_dl = NULL;
        return -1;
    }
    return 0;
}

static void
S_metal_shutdown(void)
{
    if (S_metal_dev) {
        void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
                          RTLD_LAZY | RTLD_LOCAL);
        if (cf) {
            cf_release_fn rel = (cf_release_fn)dlsym(cf, "CFRelease");
            if (rel) {
                rel(S_metal_dev);
            }
            dlclose(cf);
        }
        S_metal_dev = NULL;
    }
    if (S_metal_dl) {
        dlclose(S_metal_dl);
        S_metal_dl = NULL;
    }
}

/*
 * Vector dispatch hooks: Metal double-precision compute shaders are optional;
 * return -1 to delegate transparently to Accelerate / scalar twin.
 */
static int
S_metal_vec_add_f64(const double *a, const double *b, double *out, size_t n)
{
    (void)a; (void)b; (void)out; (void)n;
    return -1;
}

static int
S_metal_vec_mul_f64(const double *a, const double *b, double *out, size_t n)
{
    (void)a; (void)b; (void)out; (void)n;
    return -1;
}

#else /* !__APPLE__ */

static int  S_metal_probe(void)    { return 0; }
static int  S_metal_init(void)     { return -1; }
static void S_metal_shutdown(void) {}
static int  S_metal_vec_add_f64(const double *a, const double *b, double *out, size_t n)
{ (void)a; (void)b; (void)out; (void)n; return -1; }
static int  S_metal_vec_mul_f64(const double *a, const double *b, double *out, size_t n)
{ (void)a; (void)b; (void)out; (void)n; return -1; }

#endif

const perl_accel_backend_t perl_accel_backend_metal = {
    .id           = PERL_ACCEL_METAL,
    .name         = "metal",
    .description  = "macOS Apple Metal GPU runtime-probed offload backend",
    .device_class = PERL_ACCEL_DEV_GPU,
    .priority     = 60, /* below Accelerate (90) for f64 CPU/AMX workloads */
    .caps         = PERL_ACCEL_CAP_VEC_ADD_F64 | PERL_ACCEL_CAP_VEC_MUL_F64,
    .min_len_f64  = 65536u,
    .min_len_i8   = 65536u,
    .min_len_nx   = 0,
    .probe        = S_metal_probe,
    .init         = S_metal_init,
    .shutdown     = S_metal_shutdown,
    .sum_f64_cr   = NULL,
    .dot_f64_cr   = NULL,
    .vec_add_f64  = S_metal_vec_add_f64,
    .vec_mul_f64  = S_metal_vec_mul_f64,
    .vec_axpy_f64 = NULL,
    .dot_i8_i32   = NULL,
    .compress_nx  = NULL
};

/*
 * Mesa OpenCL dynamic (dlopen) GPU offload backend for perl5-mt accel.
 *
 * STRICT VENDOR POLICY:
 *   This backend accepts ONLY AMD (PCI vendor 0x1002) and Intel (PCI vendor
 *   0x8086) OpenCL GPU devices (e.g. Mesa rusticl / Clover / Intel compute).
 *   NVIDIA / CUDA devices (PCI vendor 0x10DE or any vendor/platform string
 *   containing "nvidia" or "cuda") are UNCONDITIONALLY REJECTED.
 */

#include "accel_internal.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PERL_PCI_VENDOR_AMD     0x1002u
#define PERL_PCI_VENDOR_INTEL   0x8086u
#define PERL_PCI_VENDOR_DENIED  0x10DEu /* rejected vendor ID */

static int
S_contains_icase(const char *haystack, const char *needle)
{
    size_t nlen;
    const char *p;

    if (!haystack || !needle || !*needle) {
        return 0;
    }
    nlen = strlen(needle);
    for (p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) {
            return 1;
        }
    }
    return 0;
}

/*
 * Vendor policy filter: returns 1 if and only if the OpenCL device belongs
 * to an allowed AMD or Intel GPU and does not match any denied vendor.
 */
int
perl_accel_mesa_opencl_vendor_allowed(uint32_t vendor_id,
                                      const char *vendor_name,
                                      const char *platform_name)
{
    /* Unconditional deny-list check first (even if vendor_id is spoofed) */
    if (vendor_id == PERL_PCI_VENDOR_DENIED) {
        return 0;
    }
    if (S_contains_icase(vendor_name, "nvidia") ||
        S_contains_icase(vendor_name, "cuda") ||
        S_contains_icase(platform_name, "nvidia") ||
        S_contains_icase(platform_name, "cuda")) {
        return 0;
    }

    /* Strict allow-list: AMD (0x1002) and Intel (0x8086) only */
    if (vendor_id == PERL_PCI_VENDOR_AMD || vendor_id == PERL_PCI_VENDOR_INTEL) {
        return 1;
    }

    /* If vendor_id is 0 (unknown/unreported by ICD), verify allow-listed strings */
    if (vendor_id == 0) {
        int vendor_ok =
            S_contains_icase(vendor_name, "AMD") ||
            S_contains_icase(vendor_name, "Advanced Micro Devices") ||
            S_contains_icase(vendor_name, "Intel") ||
            S_contains_icase(vendor_name, "Mesa");
        int platform_ok =
            !platform_name || !*platform_name ||
            S_contains_icase(platform_name, "Mesa") ||
            S_contains_icase(platform_name, "rusticl") ||
            S_contains_icase(platform_name, "Clover") ||
            S_contains_icase(platform_name, "AMD") ||
            S_contains_icase(platform_name, "Intel");
        if (vendor_ok && platform_ok) {
            return 1;
        }
    }

    return 0;
}

#if defined(__linux__)
#  include <dlfcn.h>

/* Minimal OpenCL C ABI definitions (no external OpenCL headers required) */
typedef int32_t  cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_bitfield;
typedef cl_bitfield cl_device_type;
typedef void    *cl_platform_id;
typedef void    *cl_device_id;

#  define CL_SUCCESS               0
#  define CL_DEVICE_TYPE_GPU       (1ULL << 2)
#  define CL_PLATFORM_NAME         0x0902u
#  define CL_DEVICE_VENDOR_ID      0x1001u
#  define CL_DEVICE_VENDOR         0x102Cu

typedef cl_int (*pfn_clGetPlatformIDs)(cl_uint, cl_platform_id *, cl_uint *);
typedef cl_int (*pfn_clGetPlatformInfo)(cl_platform_id, cl_uint, size_t, void *, size_t *);
typedef cl_int (*pfn_clGetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id *, cl_uint *);
typedef cl_int (*pfn_clGetDeviceInfo)(cl_device_id, cl_uint, size_t, void *, size_t *);

static void          *S_ocl_dl = NULL;
static cl_platform_id S_ocl_plat = NULL;
static cl_device_id   S_ocl_dev = NULL;

static int
S_mesa_opencl_find_device(void *dl, cl_platform_id *out_plat, cl_device_id *out_dev)
{
    pfn_clGetPlatformIDs  fn_plats;
    pfn_clGetPlatformInfo fn_pinfo;
    pfn_clGetDeviceIDs    fn_devs;
    pfn_clGetDeviceInfo   fn_dinfo;
    cl_platform_id plats[16];
    cl_uint nplats = 0, pi;

    fn_plats = (pfn_clGetPlatformIDs)dlsym(dl, "clGetPlatformIDs");
    fn_pinfo = (pfn_clGetPlatformInfo)dlsym(dl, "clGetPlatformInfo");
    fn_devs  = (pfn_clGetDeviceIDs)dlsym(dl, "clGetDeviceIDs");
    fn_dinfo = (pfn_clGetDeviceInfo)dlsym(dl, "clGetDeviceInfo");
    if (!fn_plats || !fn_pinfo || !fn_devs || !fn_dinfo) {
        return 0;
    }

    if (fn_plats(16, plats, &nplats) != CL_SUCCESS || nplats == 0) {
        return 0;
    }
    if (nplats > 16) nplats = 16;

    for (pi = 0; pi < nplats; pi++) {
        char pname[256] = { 0 };
        cl_device_id devs[16];
        cl_uint ndevs = 0, di;

        (void)fn_pinfo(plats[pi], CL_PLATFORM_NAME, sizeof(pname) - 1, pname, NULL);
        if (fn_devs(plats[pi], CL_DEVICE_TYPE_GPU, 16, devs, &ndevs) != CL_SUCCESS || ndevs == 0) {
            continue;
        }
        if (ndevs > 16) ndevs = 16;

        for (di = 0; di < ndevs; di++) {
            cl_uint vid = 0;
            char vname[256] = { 0 };

            (void)fn_dinfo(devs[di], CL_DEVICE_VENDOR_ID, sizeof(vid), &vid, NULL);
            (void)fn_dinfo(devs[di], CL_DEVICE_VENDOR, sizeof(vname) - 1, vname, NULL);

            if (perl_accel_mesa_opencl_vendor_allowed(vid, vname, pname)) {
                if (out_plat) *out_plat = plats[pi];
                if (out_dev)  *out_dev  = devs[di];
                return 1;
            }
        }
    }
    return 0;
}

static void *
S_mesa_opencl_dlopen(void)
{
    void *h = dlopen("libMesaOpenCL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        h = dlopen("libOpenCL.so.1", RTLD_LAZY | RTLD_LOCAL);
    }
    return h;
}

static int
S_mesa_opencl_probe(void)
{
    void *h = S_mesa_opencl_dlopen();
    int found;

    if (!h) {
        return 0;
    }
    found = S_mesa_opencl_find_device(h, NULL, NULL);
    dlclose(h);
    return found;
}

static int
S_mesa_opencl_init(void)
{
    if (S_ocl_dev) {
        return 0;
    }
    S_ocl_dl = S_mesa_opencl_dlopen();
    if (!S_ocl_dl) {
        return -1;
    }
    if (!S_mesa_opencl_find_device(S_ocl_dl, &S_ocl_plat, &S_ocl_dev)) {
        dlclose(S_ocl_dl);
        S_ocl_dl = NULL;
        return -1;
    }
    return 0;
}

static void
S_mesa_opencl_shutdown(void)
{
    S_ocl_plat = NULL;
    S_ocl_dev = NULL;
    if (S_ocl_dl) {
        dlclose(S_ocl_dl);
        S_ocl_dl = NULL;
    }
}

#else /* !__linux__ */

static int  S_mesa_opencl_probe(void)    { return 0; }
static int  S_mesa_opencl_init(void)     { return -1; }
static void S_mesa_opencl_shutdown(void) {}

#endif

static int
S_mesa_opencl_vec_add_f64(const double *a, const double *b, double *out, size_t n)
{
    (void)a; (void)b; (void)out; (void)n;
    return -1; /* fallback to CPU twin until OpenCL kernel queue is attached */
}

static int
S_mesa_opencl_vec_mul_f64(const double *a, const double *b, double *out, size_t n)
{
    (void)a; (void)b; (void)out; (void)n;
    return -1;
}

const perl_accel_backend_t perl_accel_backend_mesa_opencl = {
    .id           = PERL_ACCEL_MESA_OPENCL,
    .name         = "mesa_opencl",
    .description  = "Mesa OpenCL GPU offload (AMD 0x1002 & Intel 0x8086 only)",
    .device_class = PERL_ACCEL_DEV_GPU,
    .priority     = 70,
    .caps         = PERL_ACCEL_CAP_VEC_ADD_F64 | PERL_ACCEL_CAP_VEC_MUL_F64,
    .min_len_f64  = 65536u,
    .min_len_i8   = 65536u,
    .min_len_nx   = 0,
    .probe        = S_mesa_opencl_probe,
    .init         = S_mesa_opencl_init,
    .shutdown     = S_mesa_opencl_shutdown,
    .sum_f64_cr   = NULL,
    .dot_f64_cr   = NULL,
    .vec_add_f64  = S_mesa_opencl_vec_add_f64,
    .vec_mul_f64  = S_mesa_opencl_vec_mul_f64,
    .vec_axpy_f64 = NULL,
    .dot_i8_i32   = NULL,
    .compress_nx  = NULL
};

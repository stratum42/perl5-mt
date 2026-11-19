/*
 * Unit tests for perl_accel registry, PERL_ACCEL=auto|off|<backend>,
 * threshold-based transparent offload dispatch, and scalar fallback.
 */

#include "accel_test.h"
#include "perl_accel.h"

#include <stdlib.h>

int
main(void)
{
    const perl_accel_backend_t *list[16];
    const perl_accel_backend_t *act;
    perl_accel_stats_t st;
    size_t count;
    double a[8192], b[8192], out[8192];
    size_t i;

    for (i = 0; i < 8192; i++) {
        a[i] = (double)(i + 1) * 0.25;
        b[i] = (double)(8192 - i) * 0.125;
    }

    /* 1. Registry lists all 9 built-in backends */
    unsetenv("PERL_ACCEL");
    unsetenv("PERL_FORCE_SCALAR");
    unsetenv("PERL_ACCEL_THRESHOLD");
    perl_accel_reset_for_test("auto", (size_t)-1);

    count = perl_accel_list(list, 16);
    is_int(count, 9, "all 9 built-in backends registered");
    ok(perl_accel_find("scalar") != NULL,      "found scalar backend");
    ok(perl_accel_find("accelerate") != NULL,  "found accelerate backend");
    ok(perl_accel_find("metal") != NULL,       "found metal backend");
    ok(perl_accel_find("ane") != NULL,         "found ane backend");
    ok(perl_accel_find("mesa_opencl") != NULL, "found mesa_opencl backend");
    ok(perl_accel_find("intel_npu") != NULL,   "found intel_npu backend");
    ok(perl_accel_find("amd_npu") != NULL,     "found amd_npu backend");
    ok(perl_accel_find("power_nx") != NULL,    "found power_nx backend");
    ok(perl_accel_find("s390x_nnpa") != NULL,  "found s390x_nnpa backend");
    ok(perl_accel_find("nonexistent") == NULL, "unknown backend returns NULL");

#if defined(__APPLE__)
    act = perl_accel_active();
    is_str(act->name, "accelerate", "auto mode selects accelerate on macOS");
    is_int(perl_accel_is_enabled(), 1, "accel enabled in auto mode");

    /* 2. Below threshold -> transparently uses scalar twin */
    perl_accel_reset_stats();
    (void)perl_accel_sum_f64_cr(a, 64);
    perl_accel_get_stats(&st);
    is_int(st.offload_calls, 0, "below threshold does not offload");
    is_int(st.fallback_calls, 1, "below threshold increments fallback count");
    is_str(st.last_backend_name, "scalar", "below threshold ran scalar twin");

    /* 3. At/above threshold -> transparently offloads to accelerate */
    perl_accel_reset_stats();
    (void)perl_accel_sum_f64_cr(a, 8192);
    perl_accel_get_stats(&st);
    is_int(st.offload_calls, 1, "above threshold offloads to accelerate");
    is_int(st.fallback_calls, 0, "above threshold did not fall back");
    is_str(st.last_backend_name, "accelerate", "above threshold ran accelerate");
#else
    act = perl_accel_active();
    ok(act != NULL, "auto mode selects a valid backend (%s)", act->name);
#endif

    /* 4. PERL_ACCEL=off forces scalar even for large arrays */
    perl_accel_reset_for_test("off", 0);
    is_int(perl_accel_is_enabled(), 0, "PERL_ACCEL=off disables offload");
    is_str(perl_accel_active()->name, "scalar", "PERL_ACCEL=off activates scalar");
    (void)perl_accel_sum_f64_cr(a, 8192);
    perl_accel_vec_add_f64(a, b, out, 8192);
    perl_accel_get_stats(&st);
    is_int(st.offload_calls, 0, "PERL_ACCEL=off performs zero offloads");
    is_int(st.fallback_calls, 2, "PERL_ACCEL=off runs scalar twin");
    is_str(st.last_backend_name, "scalar", "last backend is scalar under off");

    /* 5. Explicit backend selection: scalar */
    perl_accel_reset_for_test("scalar", 0);
    is_str(perl_accel_active()->name, "scalar", "explicit scalar selection works");

    /* 6. Explicit unknown backend gracefully falls back to scalar */
    perl_accel_reset_for_test("no_such_backend", 0);
    is_str(perl_accel_active()->name, "scalar", "unknown backend falls back to scalar");

    /* 7. Environment variable PERL_ACCEL=off respected by perl_accel_init() */
    perl_accel_shutdown();
    setenv("PERL_ACCEL", "off", 1);
    perl_accel_init();
    is_int(perl_accel_is_enabled(), 0, "env PERL_ACCEL=off disables offload");
    is_str(perl_accel_active()->name, "scalar", "env PERL_ACCEL=off selects scalar");
    unsetenv("PERL_ACCEL");

    perl_accel_shutdown();
    done_testing();
}

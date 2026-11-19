/*
 * Unit tests for Mesa OpenCL vendor filter policy:
 *   - AMD (0x1002) and Intel (0x8086) GPUs MUST be accepted.
 *   - NVIDIA / CUDA (0x10DE or "nvidia"/"cuda" vendor/platform strings) MUST
 *     be unconditionally rejected, even if vendor ID is spoofed.
 *   - Unknown third-party vendors MUST be rejected.
 */

#include "accel_test.h"
#include "perl_accel.h"

int
main(void)
{
    /* Allowed AMD devices */
    is_int(perl_accel_mesa_opencl_vendor_allowed(0x1002u, "AMD", "rusticl"), 1,
           "AMD 0x1002 + rusticl accepted");
    is_int(perl_accel_mesa_opencl_vendor_allowed(0x1002u, "Advanced Micro Devices, Inc.", "Clover"), 1,
           "AMD 0x1002 + Clover accepted");
    is_int(perl_accel_mesa_opencl_vendor_allowed(0x1002u, "Mesa/X.org", "Mesa"), 1,
           "AMD 0x1002 + Mesa accepted");
    is_int(perl_accel_mesa_opencl_vendor_allowed(0u, "Advanced Micro Devices", "Mesa"), 1,
           "AMD string fallback with Mesa platform accepted");

    /* Allowed Intel devices */
    is_int(perl_accel_mesa_opencl_vendor_allowed(0x8086u, "Intel(R) Corporation", "rusticl"), 1,
           "Intel 0x8086 + rusticl accepted");
    is_int(perl_accel_mesa_opencl_vendor_allowed(0x8086u, "Intel(R) OpenCL Graphics", "Intel(R) OpenCL"), 1,
           "Intel 0x8086 + Intel OpenCL accepted");
    is_int(perl_accel_mesa_opencl_vendor_allowed(0u, "Intel", "Mesa"), 1,
           "Intel string fallback with Mesa platform accepted");

    /* Unconditionally rejected: NVIDIA / CUDA in every possible form */
    is_int(perl_accel_mesa_opencl_vendor_allowed(0x10DEu, "NVIDIA Corporation", "NVIDIA CUDA"), 0,
           "NVIDIA 0x10DE + NVIDIA CUDA rejected");
    is_int(perl_accel_mesa_opencl_vendor_allowed(0x10DEu, "Mesa", "rusticl"), 0,
           "NVIDIA PCI vendor 0x10DE under Mesa/rusticl rejected");
    is_int(perl_accel_mesa_opencl_vendor_allowed(0x1002u, "NVIDIA Corporation", "rusticl"), 0,
           "Spoofed AMD ID with NVIDIA vendor string rejected");
    is_int(perl_accel_mesa_opencl_vendor_allowed(0x8086u, "Intel", "NVIDIA CUDA"), 0,
           "Spoofed Intel ID with NVIDIA CUDA platform string rejected");
    is_int(perl_accel_mesa_opencl_vendor_allowed(0u, "nvidia", "Mesa"), 0,
           "Lowercase nvidia vendor string rejected");
    is_int(perl_accel_mesa_opencl_vendor_allowed(0u, "CUDA Device", "OpenCL"), 0,
           "CUDA vendor string rejected");
    is_int(perl_accel_mesa_opencl_vendor_allowed(0x1002u, "AMD", "cuda-bridge"), 0,
           "CUDA platform string rejected even with AMD vendor ID");

    /* Unknown / unsupported vendors rejected */
    is_int(perl_accel_mesa_opencl_vendor_allowed(0x1234u, "Generic GPU", "Unknown"), 0,
           "Unknown PCI vendor 0x1234 rejected");
    is_int(perl_accel_mesa_opencl_vendor_allowed(0u, "Imagination Technologies", "PowerVR"), 0,
           "Unsupported vendor string rejected");
    is_int(perl_accel_mesa_opencl_vendor_allowed(0u, NULL, NULL), 0,
           "NULL vendor/platform with zero ID rejected");

    done_testing();
}

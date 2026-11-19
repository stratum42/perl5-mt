# perl5-mt Benchmarks

This document records reproducible measurements for **perl5-mt** using the unified
zero-dependency benchmark suite (`tools/bench-suite.pl`), alongside multi-host
comparisons against stock Perl 5.40.2.

## Reproduction

From a built `perl5-mt` tree on any supported platform (macOS, Linux, AIX, POWER, z/OS):

```sh
./perl -I. -Ilib tools/bench-suite.pl
```

Individual suites can be selected with `--suite 1,2,3,4,5,6`, and a shorter
smoke pass is available via `--quick`.

---

## Unified Benchmark Suite Results (Apple M2 Max, macOS arm64)

- **Host**: `darwin` (`macOS 27.2.1`), `darwin-thread-multi-2level`
- **CPU**: Apple M2 Max (12 physical cores, 32 GiB RAM)
- **Compiler**: `Apple clang version 21.0.0 (clang-2100.4.1.1)` (`-O2 -g`)
- **Perl**: `v5.40.2` (`perl5-mt`)
- **Active SIMD / Accel**: SIMD=`neon`, Accel=`accelerate`
- **Command**: `./perl -I. -Ilib tools/bench-suite.pl`

### Suite 1: SIMD String, UTF-8 & Regex Kernels

Strings: 1024 KiB, median of 5 runs. Active SIMD tier: `neon` vs `scalar` (`PERL_FORCE_SCALAR=1`).

| Operation | Scalar (MB/s) | SIMD (MB/s) | Speedup |
|---|---:|---:|---:|
| `ascii index` | 47662.5 | 45590.3 | **0.96x** |
| `ascii lc` | 52428.8 | 39945.8 | **0.76x** |
| `ascii length` | 10866.1 | 82241.3 | **7.57x** |
| `ascii tr_upper` | 1472.7 | 14074.8 | **9.56x** |
| `ascii uc` | 49932.2 | 29330.8 | **0.59x** |
| `ascii upgrade` | 17848.1 | 34663.7 | **1.94x** |
| `classrun rx_span` | 10754.6 | 38479.9 | **3.58x** |
| `csv split` | 382.0 | 379.2 | **0.99x** |
| `latin1 index` | 43690.7 | 43240.2 | **0.99x** |
| `latin1 lc` | 53092.5 | 36792.1 | **0.69x** |
| `latin1 length` | 5127.5 | 5377.3 | **1.05x** |
| `latin1 tr_upper` | 2754.0 | 17260.5 | **6.27x** |
| `latin1 uc` | 52428.8 | 32263.9 | **0.62x** |
| `latin1 upgrade` | 1540.9 | 1633.9 | **1.06x** |
| `mixed decode` | 1243.9 | 5289.2 | **4.25x** |
| `mixed index` | 43690.7 | 43690.7 | **1.00x** |
| `mixed lc` | 45100.0 | 37117.7 | **0.82x** |
| `mixed length` | 10106.8 | 10010.3 | **0.99x** |
| `mixed length_dec` | 9799.8 | 10330.8 | **1.05x** |
| `mixed tr_upper` | 1620.7 | 18477.1 | **11.40x** |
| `mixed uc` | 49932.2 | 37117.7 | **0.74x** |
| `mixed upgrade` | 2341.9 | 2432.9 | **1.04x** |
| `mixed valid` | 1264.5 | 5645.1 | **4.46x** |
| `rare rx_bitmap` | 1595.4 | 14716.9 | **9.22x** |
| `rare rx_range` | 2118.3 | 37117.7 | **17.52x** |
| `run rx_run` | 16644.1 | 29537.4 | **1.77x** |
| `run rx_span` | 9915.6 | 36792.1 | **3.71x** |
| `utf8 decode` | 1199.1 | 5316.0 | **4.43x** |
| `utf8 index` | 45100.0 | 47662.5 | **1.06x** |
| `utf8 lc` | 47662.5 | 39945.8 | **0.84x** |
| `utf8 length` | 10010.3 | 10010.3 | **1.00x** |
| `utf8 length_dec` | 10754.6 | 11214.7 | **1.04x** |
| `utf8 tr_upper` | 1474.8 | 19972.9 | **13.54x** |
| `utf8 uc` | 47662.5 | 39945.8 | **0.84x** |
| `utf8 upgrade` | 2109.8 | 2222.7 | **1.05x** |
| `utf8 valid` | 1353.4 | 5683.3 | **4.20x** |

*Summary: 36 kernels measured; geometric mean **1.93x**, peak **17.52x** (`rare rx_range`).*

### Suite 2: SwissTable Hashes vs Chained Hashes

Hash size: 100,000 keys, median of 5 runs (`PERL_MT_SWISS=1` vs `PERL_MT_SWISS=0`). Checksum parity: **PASS**.

| Operation | Chained (Mops/s) | SwissTable (Mops/s) | Speedup |
|---|---:|---:|---:|
| `insert` | 13.18 | 12.25 | **0.93x** |
| `hit_lookup` | 19.54 | 18.73 | **0.96x** |
| `miss_lookup` | 22.32 | 25.36 | **1.14x** |
| `iterate` | 6.65 | 7.92 | **1.19x** |
| `delete` | 10.18 | 9.09 | **0.89x** |

### Suite 3: Packed `MT::Array` + Live Hardware Offload (`MT::Accel`) vs Pure-Perl Arrays

Vector length: 131,072 elements, median of 5 runs. Active backend: `accelerate` (`offload_calls=303`, `last_backend=accelerate`).

| Operation | Pure-Perl (ms) | `MT::Array` Scalar CR (ms) | `MT::Array` + `MT::Accel` (ms) | Speedup vs Pure-Perl | Accel vs Scalar CR | Bit-Identity (Scalar CR vs Accel) |
|---|---:|---:|---:|---:|---:|---|
| `sum` | 4.398 | 2.606 | 0.369 | **11.9x** | **7.06x** | EXACT (`40de316dce1ff2e4`) |
| `dot` | 5.108 | 4.965 | 1.350 | **3.8x** | **3.68x** | EXACT (`c111700bda883b75`) |
| `vec_add` | 12.249 | 0.973 | 0.962 | **12.7x** | **1.01x** | EXACT (`40d8972a14631f89`) |
| `vec_mul` | 12.062 | 0.960 | 0.946 | **12.8x** | **1.01x** | EXACT (`c111700bda883b75`) |
| `axpy` | 12.856 | 0.959 | 0.971 | **13.2x** | **0.99x** | EXACT (`c0f353ea20d22d0d`) |
| `dot_i8` | 4.697 | 0.004 | 0.008 | **587.1x** | **0.50x** | EXACT (`-2359296`) |

### Suite 4: VM Optimizations (Freeze-Time Specialisation & Inline Caches)

Tight loops inside a frozen `MT::Image` worker comparing default specialisation (`specialize=1 meth_ic=63 helem_ic=656 spec_const=0 spec_devirt=24`) vs `PERL_MT_NO_SPECIALIZE=1`.

| Workload | `PERL_MT_NO_SPECIALIZE=1` (Mops/s) | Default Specialized (Mops/s) | Speedup | Parity |
|---|---:|---:|---:|---|
| `const_sub` | 11.32 | 10.97 | **0.97x** | PASS |
| `static_method` | 8.43 | 12.51 | **1.49x** | PASS |
| `obj_method` | 5.43 | 5.92 | **1.09x** | PASS |
| `frozen_helem` | 18.87 | 19.19 | **1.02x** | PASS |
| `mixed_hot_loop` | 3.05 | 3.67 | **1.20x** | PASS |

### Suite 5: Shared-Image Multi-Threading & Fibers vs `ithreads` vs `prefork`

#### 5a. Shared-Image Worker Pool vs `ithreads` vs `prefork`

Workload: `tools/bench/Workload.pm` with `--image-mb 32 --items 200` (median of 3 runs).

| Workers | Driver | Throughput (ops/s) | Total RSS (MiB) | RSS / Worker (MiB) | Startup (ms) |
|---:|---|---:|---:|---:|---:|
| 1 | `mt` | 439.9 | 132.2 | 35.06 | 8.70 |
| 1 | `ithreads` | 468.1 | 87.6 | 37.70 | 8.63 |
| 1 | `prefork` | 450.5 | 54.2 | 5.08 | 4.23 |
| 2 | `mt` | 860.1 | 166.9 | 34.94 | 7.98 |
| 2 | `ithreads` | 929.9 | 124.1 | 37.12 | 14.31 |
| 2 | `prefork` | 508.8 | 56.0 | 3.50 | 4.52 |
| 4 | `mt` | 1611.5 | 236.6 | 34.91 | 8.82 |
| 4 | `ithreads` | 1837.9 | 197.3 | 36.86 | 23.62 |
| 4 | `prefork` | 514.1 | 60.5 | 2.82 | 6.33 |
| 8 | `mt` | 2939.6 | 376.3 | 34.90 | 8.95 |
| 8 | `ithreads` | 3335.8 | 343.7 | 36.72 | 43.51 |
| 8 | `prefork` | 650.6 | 83.3 | 4.27 | 8.44 |

#### 5b. Cooperative Stackful Fibers (`MT::Pool` + 4 `MT::Fiber`s / Worker)

Fiber `resume`+`yield` round-trip (200,000 switches): **340.1 ns** (**2.94 M switches/s**).

| Workers | Fibers / Worker | Total Fibers | Throughput (items/s) | Scaling vs 1W |
|---:|---:|---:|---:|---:|
| 1 | 4 | 4 | 7030.5 | **1.00x** |
| 2 | 4 | 8 | 14131.6 | **2.01x** |
| 4 | 4 | 16 | 27872.6 | **3.96x** |
| 8 | 4 | 32 | 43732.1 | **6.22x** |

### Suite 6: Parallel Ndarray (`MT::PDLBridge`) & Concurrency Primitives

#### 6a. Parallel Shared-Memory Ndarray (`MT::PDLBridge`, `nelem=524288`, `workers=4`)

| Mode | Elapsed (ms) | Speedup | SHA-256 Digest Parity |
|---|---:|---:|---|
| 1-thread scalar reference | 161.80 | 1.00x | `a6c37269abe8ff4f...` |
| 4-worker shm `MT::Pool` | 4.50 | **35.96x** | **PASS** (`a6c37269abe8ff4f...`) |

#### 6b. Lock-Free `MT::Cell` & Cross-Worker `MT::Channel` Latencies

| Primitive | Operation | Throughput | Mean Latency |
|---|---|---:|---:|
| `MT::Cell` | Lock-free `cas()` (250,000 ops) | **2.96 Mops/s** | 0.337 µs |
| `MT::Cell` | Lock-free `get()` (250,000 ops) | **6.30 Mops/s** | 0.159 µs |
| `MT::Channel` | Cross-worker ping-pong RTT (50,000 trips) | **762,196 trips/s** | **1.31 µs** |

---

## Unified Benchmark Suite Results (Intel Core Ultra 7 265K, Linux x86_64)

- **Host**: `linux` (`Linux 7.2.11-1-default`), `x86_64-linux-thread-multi`
- **CPU**: Intel Core Ultra 7 265K (20 physical cores, 46 GiB RAM)
- **Compiler**: `cc (SUSE Linux) 16.2.1 20260929` (`-O2 -g`)
- **Perl**: `v5.40.2` (`perl5-mt`)
- **Active SIMD / Accel**: SIMD=`avx2`, Accel=`mesa_opencl` (scalar CR fallback)
- **Command**: `./perl -I. -Ilib tools/bench-suite.pl`

### Suite 1: SIMD String, UTF-8 & Regex Kernels

Strings: 1024 KiB, median of 5 runs. Active SIMD tier: `avx2` vs `scalar` (`PERL_FORCE_SCALAR=1`).

| Operation | Scalar (MB/s) | SIMD (MB/s) | Speedup |
|---|---:|---:|---:|
| `ascii index` | 139810.1 | 144631.2 | **1.03x** |
| `ascii lc` | 4100.0 | 35848.8 | **8.74x** |
| `ascii length` | 17848.1 | 139810.1 | **7.83x** |
| `ascii tr_upper` | 5343.1 | 14122.2 | **2.64x** |
| `ascii uc` | 4132.3 | 37117.7 | **8.98x** |
| `ascii upgrade` | 29330.8 | 41527.8 | **1.42x** |
| `classrun rx_span` | 16384.0 | 71089.9 | **4.34x** |
| `csv split` | 664.9 | 682.2 | **1.03x** |
| `latin1 index` | 139810.1 | 144631.2 | **1.03x** |
| `latin1 lc` | 4017.5 | 45590.3 | **11.35x** |
| `latin1 length` | 9098.3 | 8701.9 | **0.96x** |
| `latin1 tr_upper` | 6288.3 | 13706.9 | **2.18x** |
| `latin1 uc` | 4219.6 | 34663.7 | **8.21x** |
| `latin1 upgrade` | 2277.0 | 2385.8 | **1.05x** |
| `mixed decode` | 1825.2 | 7928.7 | **4.34x** |
| `mixed index` | 113359.6 | 144631.2 | **1.28x** |
| `mixed lc` | 4033.0 | 38479.9 | **9.54x** |
| `mixed length` | 17260.5 | 16710.4 | **0.97x** |
| `mixed length_dec` | 17848.1 | 17848.1 | **1.00x** |
| `mixed tr_upper` | 3366.2 | 14074.8 | **4.18x** |
| `mixed uc` | 4236.7 | 34663.7 | **8.18x** |
| `mixed upgrade` | 3194.4 | 3358.1 | **1.05x** |
| `mixed valid` | 1851.8 | 8774.7 | **4.74x** |
| `rare rx_bitmap` | 3266.6 | 17260.5 | **5.28x** |
| `rare rx_range` | 3609.6 | 62601.6 | **17.34x** |
| `run rx_run` | 43690.7 | 71089.9 | **1.63x** |
| `run rx_span` | 16131.9 | 71089.9 | **4.41x** |
| `utf8 decode` | 1751.3 | 7810.6 | **4.46x** |
| `utf8 index` | 113359.6 | 139810.1 | **1.23x** |
| `utf8 lc` | 4033.0 | 45100.0 | **11.18x** |
| `utf8 length` | 17260.5 | 15887.5 | **0.92x** |
| `utf8 length_dec` | 18157.2 | 17848.1 | **0.98x** |
| `utf8 tr_upper` | 2571.6 | 13888.4 | **5.40x** |
| `utf8 uc` | 4096.0 | 37117.7 | **9.06x** |
| `utf8 upgrade` | 3216.5 | 3355.4 | **1.04x** |
| `utf8 valid` | 1838.0 | 8612.5 | **4.69x** |

*Summary: 36 kernels measured; geometric mean **3.01x**, peak **17.34x** (`rare rx_range`).*

### Suite 2: SwissTable Hashes vs Chained Hashes

Hash size: 100,000 keys, median of 5 runs (`PERL_MT_SWISS=1` vs `PERL_MT_SWISS=0`). Checksum parity: **PASS**.

| Operation | Chained (Mops/s) | SwissTable (Mops/s) | Speedup |
|---|---:|---:|---:|
| `insert` | 19.84 | 15.78 | **0.80x** |
| `hit_lookup` | 26.09 | 31.21 | **1.20x** |
| `miss_lookup` | 34.69 | 41.31 | **1.19x** |
| `iterate` | 13.12 | 16.18 | **1.23x** |
| `delete` | 15.10 | 15.63 | **1.04x** |

### Suite 3: Packed `MT::Array` + Live Hardware Offload (`MT::Accel`) vs Pure-Perl Arrays

Vector length: 131,072 elements, median of 5 runs. Active backend: `mesa_opencl` (`offload_calls=0`, `last_backend=scalar`).

| Operation | Pure-Perl (ms) | `MT::Array` Scalar CR (ms) | `MT::Array` + `MT::Accel` (ms) | Speedup vs Pure-Perl | Accel vs Scalar CR | Bit-Identity (Scalar CR vs Accel) |
|---|---:|---:|---:|---:|---:|---|
| `sum` | 1.846 | 1.484 | 1.495 | **1.2x** | **0.99x** | EXACT (`40de316dce1ff2e4`) |
| `dot` | 2.727 | 3.034 | 3.059 | **0.9x** | **0.99x** | EXACT (`c111700bda883b75`) |
| `vec_add` | 5.886 | 0.635 | 0.621 | **9.5x** | **1.02x** | EXACT (`40d8972a14631f89`) |
| `vec_mul` | 5.867 | 0.620 | 0.602 | **9.7x** | **1.03x** | EXACT (`c111700bda883b75`) |
| `axpy` | 6.346 | 0.608 | 0.605 | **10.5x** | **1.00x** | EXACT (`c0f353ea20d22d0d`) |
| `dot_i8` | 2.255 | 0.004 | 0.004 | **563.8x** | **1.00x** | EXACT (`-2359296`) |

### Suite 4: VM Optimizations (Freeze-Time Specialisation & Inline Caches)

Tight loops inside a frozen `MT::Image` worker comparing default specialisation (`specialize=1 meth_ic=63 helem_ic=691 spec_const=0 spec_devirt=24`) vs `PERL_MT_NO_SPECIALIZE=1`.

| Workload | `PERL_MT_NO_SPECIALIZE=1` (Mops/s) | Default Specialized (Mops/s) | Speedup | Parity |
|---|---:|---:|---:|---|
| `const_sub` | 21.34 | 20.96 | **0.98x** | PASS |
| `static_method` | 14.97 | 23.94 | **1.60x** | PASS |
| `obj_method` | 10.11 | 11.49 | **1.14x** | PASS |
| `frozen_helem` | 41.07 | 40.67 | **0.99x** | PASS |
| `mixed_hot_loop` | 5.42 | 6.56 | **1.21x** | PASS |

### Suite 5: Shared-Image Multi-Threading & Fibers vs `ithreads` vs `prefork`

#### 5a. Shared-Image Worker Pool vs `ithreads` vs `prefork`

Workload: `tools/bench/Workload.pm` with `--image-mb 32 --items 200` (median of 3 runs).

| Workers | Driver | Throughput (ops/s) | Total RSS (MiB) | RSS / Worker (MiB) | Startup (ms) |
|---:|---|---:|---:|---:|---:|
| 1 | `mt` | 739.6 | 122.5 | 35.38 | 13.29 |
| 1 | `ithreads` | 785.4 | 80.5 | 35.83 | 15.55 |
| 1 | `prefork` | 800.4 | 88.0 | 43.92 | 2.95 |
| 2 | `mt` | 1408.8 | 157.8 | 35.30 | 13.10 |
| 2 | `ithreads` | 1452.1 | 115.3 | 35.32 | 31.52 |
| 2 | `prefork` | 783.4 | 127.3 | 41.59 | 3.19 |
| 4 | `mt` | 2489.7 | 228.2 | 35.27 | 12.71 |
| 4 | `ithreads` | 2507.7 | 184.5 | 34.94 | 60.10 |
| 4 | `prefork` | 794.0 | 206.1 | 40.47 | 3.56 |
| 8 | `mt` | 4459.7 | 369.1 | 35.24 | 12.81 |
| 8 | `ithreads` | 4529.7 | 322.8 | 34.75 | 128.26 |
| 8 | `prefork` | 783.4 | 363.3 | 39.89 | 4.26 |

#### 5b. Cooperative Stackful Fibers (`MT::Pool` + 4 `MT::Fiber`s / Worker)

Fiber `resume`+`yield` round-trip (200,000 switches): **184.2 ns** (**5.43 M switches/s**).

| Workers | Fibers / Worker | Total Fibers | Throughput (items/s) | Scaling vs 1W |
|---:|---:|---:|---:|---:|
| 1 | 4 | 4 | 13807.6 | **1.00x** |
| 2 | 4 | 8 | 25944.0 | **1.88x** |
| 4 | 4 | 16 | 44766.9 | **3.24x** |
| 8 | 4 | 32 | 82328.4 | **5.96x** |

### Suite 6: Parallel Ndarray (`MT::PDLBridge`) & Concurrency Primitives

#### 6a. Parallel Shared-Memory Ndarray (`MT::PDLBridge`, `nelem=524288`, `workers=4`)

| Mode | Elapsed (ms) | Speedup | SHA-256 Digest Parity |
|---|---:|---:|---|
| 1-thread scalar reference | 88.80 | 1.00x | `a6c37269abe8ff4f...` |
| 4-worker shm `MT::Pool` | 4.10 | **21.66x** | **PASS** (`a6c37269abe8ff4f...`) |

#### 6b. Lock-Free `MT::Cell` & Cross-Worker `MT::Channel` Latencies

| Primitive | Operation | Throughput | Mean Latency |
|---|---|---:|---:|
| `MT::Cell` | Lock-free `cas()` (250,000 ops) | **4.67 Mops/s** | 0.214 µs |
| `MT::Cell` | Lock-free `get()` (250,000 ops) | **8.48 Mops/s** | 0.118 µs |
| `MT::Channel` | Cross-worker ping-pong RTT (50,000 trips) | **676,003 trips/s** | **1.48 µs** |

---

## Cross-Host Bit-Identical Determinism Verification (macOS `arm64` vs Linux `x86_64`)

Running `./perl -I. -Ilib t/mt/num_digest.t` on both Apple M2 Max (`macOS arm64`, Apple clang 21.0.0) and Intel Core Ultra 7 265K (`Linux x86_64`, GCC 16.2.1) produces bit-for-bit identical IEEE-754 binary64, transcendental, NaN, and `MT::Decimal` SHA-256 digests:

| Digest Component | macOS `arm64` (M2 Max) | Linux `x86_64` (Core Ultra 7 265K) | Parity |
|---|---|---|---|
| `arith_digest` | `8e61688b9ebd4392a04fb81542767f5a76fc5aeed83149e2356ebf35c85281da` | `8e61688b9ebd4392a04fb81542767f5a76fc5aeed83149e2356ebf35c85281da` | **EXACT** |
| `parse_digest` | `aa78e4c2f7029b1e226e601986f597da4cde9b94117d753baeda195c957bd630` | `aa78e4c2f7029b1e226e601986f597da4cde9b94117d753baeda195c957bd630` | **EXACT** |
| `fmt_digest` | `bd0fbf4396ac2e4f707c63d2b19fadc032ea291abaa908894acc1ac876c4edff` | `bd0fbf4396ac2e4f707c63d2b19fadc032ea291abaa908894acc1ac876c4edff` | **EXACT** |
| `nan_digest` | `0233909d884870074d1983c82f3c997979019529d71cd9b893d75d7ae121d94e` | `0233909d884870074d1983c82f3c997979019529d71cd9b893d75d7ae121d94e` | **EXACT** |
| `libm_digest` | `8c4e636bcd84c17bfa1a1176bba32968a5e27ddf017199e29ffd72dfe8fbe582` | `8c4e636bcd84c17bfa1a1176bba32968a5e27ddf017199e29ffd72dfe8fbe582` | **EXACT** |
| `decimal_digest` | `6f644bbbaca5b894c481e744049af1fe0ba6cd7a815b11b2ca5098e8624e41b2` | `6f644bbbaca5b894c481e744049af1fe0ba6cd7a815b11b2ca5098e8624e41b2` | **EXACT** |
| **`overall_numeric_digest`** | **`3cc524ed2f4a700c9d6bfe6047d8d6cecfaea84c3e7fe74747ad4f9e70d0271b`** | **`3cc524ed2f4a700c9d6bfe6047d8d6cecfaea84c3e7fe74747ad4f9e70d0271b`** | **EXACT** |

All six Suite 3 `MT::Array` reduction/vector bit-patterns (`sum`, `dot`, `vec_add`, `vec_mul`, `axpy`, `dot_i8`) and the Suite 6a `MT::PDLBridge` parallel ndarray SHA-256 digest (`a6c37269abe8ff4f...`) likewise match bit-for-bit across macOS `arm64` and Linux `x86_64`.

---

## PGO + ThinLTO + Native Build Comparison (`-O2 -g` vs `-Dusepgo -Dusenative`, Apple M2 Max)

Head-to-head comparison on the same Apple M2 Max (`macOS 27.2.1`, `arm64`, `Apple clang version 21.0.0`) host running `./perl -I. -Ilib tools/bench-suite.pl`:

- **Baseline (`-O2 -g`)**: `./Configure -des -Dusethreads`
- **PGO + Native**: `./Configure -des -Dusethreads -Dusepgo -Dusenative && make pgo` (`-O3 -g -march=native -flto=thin -fprofile-instr-use=pgo-prof/default.profdata -ffp-contract=off -Wl,-export_dynamic`)

### Unified Suite Head-to-Head Summary

| Suite | Workload | `-O2 -g` | `PGO + ThinLTO + native` | Speedup |
|---|---|---:|---:|---:|
| Suite 1 | `csv split` (SIMD, MB/s) | 379.2 | 558.6 | **1.47x** |
| Suite 1 | `ascii tr_upper` (SIMD, MB/s) | 14074.8 | 19599.6 | **1.39x** |
| Suite 1 | `ascii uc` (SIMD, MB/s) | 29330.8 | 36792.1 | **1.25x** |
| Suite 1 | `ascii length` (SIMD, MB/s) | 82241.3 | 89240.5 | **1.09x** |
| Suite 2 | SwissTable `insert` (Mops/s) | 12.25 | 14.88 | **1.21x** |
| Suite 2 | SwissTable `hit_lookup` (Mops/s) | 18.73 | 20.09 | **1.07x** |
| Suite 2 | SwissTable `miss_lookup` (Mops/s) | 25.36 | 27.03 | **1.07x** |
| Suite 2 | SwissTable `iterate` (Mops/s) | 7.92 | 8.50 | **1.07x** |
| Suite 2 | SwissTable `delete` (Mops/s) | 9.09 | 10.03 | **1.10x** |
| Suite 3 | Pure-Perl `sum` (131k, ms, lower is better) | 4.398 | 2.908 | **1.51x** |
| Suite 3 | Pure-Perl `dot` (131k, ms, lower is better) | 5.108 | 4.031 | **1.27x** |
| Suite 3 | Pure-Perl `vec_add` (131k, ms, lower is better) | 12.249 | 9.100 | **1.35x** |
| Suite 3 | Pure-Perl `vec_mul` (131k, ms, lower is better) | 12.062 | 8.968 | **1.35x** |
| Suite 3 | Pure-Perl `axpy` (131k, ms, lower is better) | 12.856 | 9.630 | **1.33x** |
| Suite 3 | Pure-Perl `dot_i8` (131k, ms, lower is better) | 4.697 | 3.564 | **1.32x** |
| Suite 4 | VM `const_sub` (specialized, Mops/s) | 10.97 | 12.73 | **1.16x** |
| Suite 4 | VM `static_method` (specialized, Mops/s) | 12.51 | 14.32 | **1.14x** |
| Suite 4 | VM `obj_method` (specialized, Mops/s) | 5.92 | 7.10 | **1.20x** |
| Suite 4 | VM `frozen_helem` (specialized, Mops/s) | 19.19 | 22.60 | **1.18x** |
| Suite 4 | VM `mixed_hot_loop` (specialized, Mops/s) | 3.67 | 4.35 | **1.19x** |
| Suite 5a | `mt` pool 1W (ops/s) | 439.9 | 534.4 | **1.21x** |
| Suite 5a | `mt` pool 2W (ops/s) | 860.1 | 1017.3 | **1.18x** |
| Suite 5a | `mt` pool 4W (ops/s) | 1611.5 | 1936.1 | **1.20x** |
| Suite 5a | `mt` pool 8W (ops/s) | 2939.6 | 3614.3 | **1.23x** |
| Suite 5b | Fiber `resume`+`yield` RTT (ns, lower is better) | 340.1 | 307.9 | **1.10x** |
| Suite 5b | 8W x 4 fibers = 32 fibers (items/s) | 43732.1 | 61905.8 | **1.42x** |
| Suite 6b | `MT::Cell cas()` (Mops/s) | 2.96 | 3.54 | **1.20x** |
| Suite 6b | `MT::Cell get()` (Mops/s) | 6.30 | 8.65 | **1.37x** |
| Suite 6b | `MT::Channel` ping-pong RTT (trips/s / µs) | 762,196 (1.31 µs) | 990,587 (1.01 µs) | **1.30x** |

### Numeric Determinism & Correctness Parity Under PGO + ThinLTO + Native

- **`t/mt/num_digest.t`**: **93/93 PASS** with exact `overall_numeric_digest=3cc524ed2f4a700c9d6bfe6047d8d6cecfaea84c3e7fe74747ad4f9e70d0271b` (bit-for-bit identical to `-O2 -g` macOS `arm64` and Linux `x86_64`, proving `-ffp-contract=off` is preserved under `-O3`, ThinLTO, and PGO).
- **Suite 3 & Suite 6a bit-identity**: All six `MT::Array` reduction/vector digests (`sum`, `dot`, `vec_add`, `vec_mul`, `axpy`, `dot_i8`) and the `MT::PDLBridge` parallel ndarray digest (`a6c37269abe8ff4f...`) match bit-for-bit.
- **MT test suite**: `mt/hv_swiss.t`, `mt/av_packed.t`, `mt/accel_array.t`, `mt/vm_opt.t`, `mt/fiber.t`, `mt/fault.t`, `mt/partition.t`, and `mt/build_flags.t`: **8/8 files, 378/378 tests PASS**.

---

# Multi-Host Driver Comparison vs Stock Perl 5.40.2

Three drivers run the same workload (`tools/bench/Workload.pm`), so the rows
compare how the work is spread over the processes or threads, not the work:

- **stock prefork** (`tools/bench/prefork.pl`): N forked processes read item
  numbers from one pipe and write checksums to another.  Stock 5.40.2.
- **stock ithreads** (`tools/bench/ithreads.pl`): N `threads` take items from a
  `Thread::Queue` and return checksums on another.  Stock 5.40.2.
- **perl5-mt pool** (`tools/bench/mt.pl`): `MT::Pool` with N workers; the items
  go through the pool's `map`.  The perl5-mt build.

One item of work: a 16 KiB file read (through `:async` under perl5-mt, through
PerlIO under stock), a socketpair echo, a round trip of a 10 KiB document with
Cpanel::JSON::XS (through a compartment under perl5-mt, in-process under stock),
and a 1500-step pure-Perl hashing loop.

Each number is the median of three runs of 2000 items.  N is half the number of
cores (4, 16, 14).  `rss_kb` is the resident set of the whole process tree after
a warm-up (`ps -o rss`); the threaded drivers are one process.  `startup_ms` is
the time from spawning the workers to the first result (the ithreads and
prefork rows: the first `start` a worker reports; the pool row: a single job
run on a fresh pool).

## Intel Core i7-1185G7 (Linux x86_64, 8 cores, 4 workers)

| driver | ops/s | rss_kb | startup_ms |
|---|---:|---:|---:|
| stock prefork | 4923.6 | 39624 | 1.13 |
| stock ithreads | 8021.4 | 20908 | 10.05 |
| perl5-mt pool | 7697.8 | 31568 | 1.67 |

## IBM POWER8 (Linux ppc64le, 32 cores, 16 workers)

| driver | ops/s | rss_kb | startup_ms |
|---|---:|---:|---:|
| stock prefork | 1022.7 | 182592 | 16.87 |
| stock ithreads | 2820.1 | 51264 | 116.86 |
| perl5-mt pool | 3269.6 | 80832 | 7.56 |

## Apple M3 Ultra (macOS arm64, 28 cores, 14 workers)

| driver | ops/s | rss_kb | startup_ms |
|---|---:|---:|---:|
| stock prefork | 10586.4 | 68640 | 10.19 |
| stock ithreads | 27106.5 | 40080 | 17.38 |
| perl5-mt pool | 22353.8 | 35168 | 1.60 |

## Fiber and Remote XS Call Micro-Benchmarks

A remote XS call (`tools/mt-xs-latency.pl`, round trip, median of five rounds): the
thread row blocks on the reply vector with the adaptive wait policy; the one-fiber
row parks the fiber and the scheduler's drain wakes it; the baseline column shows the
earlier non-vector bridge implementation.

| Host | Remote call, thread (adaptive) | Remote call, one fiber | Baseline thread bridge |
|---|---:|---:|---:|
| Intel Core i7-1185G7 (Linux x86_64) | 1.57 µs | 1.81 µs | 7.11 µs |
| IBM POWER8 (Linux ppc64le) | 10.4 µs | 10.57 µs | 24.5 µs |
| Apple M3 Ultra (macOS arm64) | 1.06 µs | 1.07 µs | 9.4 µs |

The ppc64le adaptive figure reflects the adaptive poll-budget learning fix; the
one-fiber figure there predates it.

## What the numbers show, and what they do not

They show that the pool's per-item cost is comparable to the stock forms on
each host.  On amd64 and ppc64le the pool beats stock prefork in ops/s, and it
starts its first job faster than stock ithreads.  On macOS the stock ithreads
driver is faster than the pool (27k against 22k ops/s), and the pool uses less
memory than stock prefork in every row; against stock ithreads it uses less
memory only on macOS (on amd64 and ppc64le the threaded stock driver is the
smaller one).

They do not show much more than that.  The item is a mix dominated by the JSON
round trip and the hashing loop, which are pure Perl or XS work that runs at the
same speed in every driver; the file read and the socketpair are a small part
of it.  The pool's JSON call goes through a compartment (an out-of-process XS
module) rather than in-process, which costs a round trip per call; the stock
drivers call Cpanel::JSON::XS in-process.  So the ops/s rows mostly measure how
the drivers schedule the same CPU work, and the compartment's cost is inside the
pool's numbers.  The startup rows measure one spawn on a quiet machine, not a
steady state.  The RSS rows are a single warm-up snapshot; a fork that copies
pages it writes would show more under load.  Three repetitions of one size are
not a sweep: a different item count or worker count can move the order of the
rows.  The micro-numbers are from earlier measurements and were not re-measured here.

## Additional Benchmark & Verification Harnesses

Beyond the unified suite and driver comparison above, the repository includes
focused micro-benchmarks and verification harnesses for each subsystem:

### SIMD String, UTF-8, Byte & Regex Kernels

Run `tools/simd-bench.pl --reps 9` to compare the active SIMD tier against
`PERL_FORCE_SCALAR=1` across ASCII, Latin-1, mixed, and multi-byte UTF-8
inputs (`length`, `utf8::upgrade`, `utf8::valid`, `utf8::decode`, `uc`, `lc`,
`tr/a-z/A-Z/`, and regex span/bitmap/range matches).

### SwissTable Hashes & Packed Typed Arrays

- `t/mt/hv_swiss.t`: deterministic 200,000-operation mixed insert/lookup/
  exists/delete/iterate workload verifying `HV` parity with `Hash::Util` and
  `Storable`.
- `t/mt/av_packed.t`: packed `IV`, `NV`, and `U8` array operations and
  `MT::Array` reductions (`sum`, `min`, `max`, `dot`, `map_add`, `slice`,
  `count`, `find`) verified against scalar twins.

### Deterministic Numerics & Acceleration Dispatch

- `t/mt/num_digest.t`: cross-host SHA-256 numeric digest covering contraction
  control (`-ffp-contract=off`), Eisel-Lemire parsing, Ryu shortest round-trip
  (`PERL_NV_SHORTEST=1`), correctly-rounded `core-math` transcendentals, NaN
  canonicalisation, and `MT::Decimal` decimal128 operations.
- `make -C accel test` and `t/mt/accel.t`: backend registry probing, Ozaki
  correctly-rounded reduction parity on macOS Accelerate, Mesa OpenCL vendor
  filtering, and `PERL_ACCEL=off` / `PERL_FORCE_SCALAR=1` fallback parity.

### Core VM Optimizations & PGO

- `t/mt/vm_opt.t`: verifies RC-stack PADTMP elimination, `entersub`/`leavesub`
  fast paths, peephole superinstructions with `B::Deparse` equivalence,
  overlay-validated inline caches, and freeze-time specialisation parity
  against `PERL_MT_NO_SPECIALIZE=1`.
- `tools/pgo-train.pl`: exercises the representative workload corpus during
  `make pgo` (`-Dusepgo`, optional `-Dusenative`, `-Dusesymbolbinding`, and
  `-Dusehugepagetext`) for Profile-Guided Optimization builds.
- `t/mt/build_flags.t`: validates PGO/native/symbol-binding/hugepage flags and
  ensures `-ffp-contract=off` is preserved and `-ffast-math` is absent.

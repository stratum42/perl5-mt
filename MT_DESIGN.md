# perl5-mt Design & Architecture

`perl5-mt` is a standalone fork of upstream Perl 5.40.2 built to give Perl
shared-memory multi-threading, stackful fibers, deterministic numerics, and
dual-gated SIMD/hardware offload without breaking existing Perl 5 code or
CPAN modules.

---

## 1. Design Invariants

Four invariants apply across the codebase:

1. **Upstream Perl 5.40.2 Compatibility**: Existing pure-Perl programs and XS
   modules continue to run unchanged.  Single-threaded code pays minimal
   overhead, `B::Deparse` output is preserved across VM optimizations, and
   default numeric formatting (`%.15g`) remains unchanged unless opted in.
2. **Dual-Gated Parity**: Every SIMD kernel (`simd/`), accelerated offload
   backend (`accel/`), and freeze-time VM specialisation has a reference scalar
   or unspecialised twin.  Runtime switches (`PERL_FORCE_SCALAR=1`,
   `PERL_ACCEL=off`, `PERL_MT_NO_SPECIALIZE=1`) and compile-time gates
   (`-DPERL_FORCE_SCALAR`) force the reference path so parity tests can prove
   bit-for-bit equivalence.
3. **Cross-Host Deterministic Numerics**: Floating-point contraction is
   disabled (`-ffp-contract=off`), `-ffast-math` is prohibited, string-to-float
   parsing and correctly-rounded `core-math` transcendentals produce identical
   IEEE 754 binary64 bit patterns across architectures, NaN payloads are
   canonicalised to quiet positive NaN (`0x7ff8000000000000`), and
   `MT::Decimal` provides 34-digit IEEE 754-2008 decimal128 with canonical
   16-byte BID128 serialization.
4. **Fail-Fast Fault Containment**: Accidental mutation of the sealed boot
   image traps at the offending instruction (`SEGV_PKUERR` / `SIGSEGV`) or
   croaks `MT_IMMORTAL_MUTATION`.  Legacy XS crashes are isolated to
   out-of-process compartments.  Workers and compartments report liveness via
   lock-free vector heartbeats (`MT::Supervisor`) and surface structured fault
   records (`MT::Fault`).

---

## 2. System Architecture

```
+-----------------------------------------------------------------------------+
| Perl 5 Applications & CPAN Modules                                          |
+-----------------------------------------------------------------------------+
| Public MT::* APIs (ext/MT/, ext/MT-Decimal/)                                |
|   MT::Image   MT::Worker   MT::Pool     MT::Fiber    MT::IO (:async)        |
|   MT::Cap     MT::Channel  MT::Cell     MT::SharedHash                      |
|   MT::Array   MT::Decimal  MT::Fault    MT::Supervisor                      |
+-----------------------------------------------------------------------------+
| Core VM & Multi-Threading Runtime                                           |
|   Frozen Boot Image (mt_freeze.c, SVf_IMMORTAL, per-worker overlays/GPs)    |
|   Two-Tier XS Dispatcher & Compartments (mt_xs_bridge.c, mt_xs_compartment) |
|   Stackful Fibers & Scheduler (mt_fiber.c, mt_sched.c, mt_ctx_*.c)          |
|   VM Optimizations (op.c, pp_hot.c, inline caches, freeze specialization)   |
|   Fault & Liveness Engine (mt_fault.c)                                      |
+-----------------------------------------------------------------------------+
| Data Structures, Numerics & Acceleration                                    |
|   SwissTable HV (hv.c) | Packed AV (av.c) | core-math/ (cr_libm, parse, ryu)|
|   simd/ (SSE2, AVX2, AVX-512, VNNI, NEON, VSX/VSX2/VSX3, s390x VX/NNPA)     |
|   accel/ (Accelerate, Metal, ANE, Mesa OpenCL, Intel/AMD NPU, POWER NX,     |
|           s390x NNPA, scalar reference twin)                                |
+-----------------------------------------------------------------------------+
| krt/ Kernel Runtime Adapter (No Perl header dependencies)                   |
|   krt_linux.c | krt_darwin.c | krt_aix.c (AIX + PASE) | krt_zos.c (USS)     |
|   Sealable SHM | Compartments | Futex/Ulock | rseq/CPU | io_uring/kqueue/   |
|   pollset/poll | Lock-Free SPSC/MPSC Rings | krt_vec Doorbell Waiters       |
+-----------------------------------------------------------------------------+
```

---

## 3. Frozen Boot Image & Shared-Memory Workers

After `BEGIN` and `INIT` complete, `MT::Image::freeze()` (or automatic freezing
under `PERL_MT_IMAGE=1`) clones the compiled interpreter into a sealable
`perl_krt_shm_t` arena (`PERL_MT_ARENA_MB`, default 512 MiB).

- **Immortality & Sealing**: Every SV in the frozen clone is marked
  `SVf_IMMORTAL | SVf_READONLY | SVf_PROTECT` with a saturated reference count.
  The arena is sealed using Linux memory protection keys (`pkey_mprotect`,
  re-armed `NOWRITE` at every `JMPENV_PUSH` landing) or `mprotect`/`mach_vm_protect`
  on platforms without pkeys.
- **Branch-Free Refcount Guards**: `SvREFCNT_inc`/`SvREFCNT_dec` redirect
  immortal reference count updates to a per-interpreter sink
  (`PL_mt_refcnt_sink`) via a branch-free conditional select (`cmov`, `csel`,
  `isel`).
- **Per-Worker Isolation**: Each worker (`MT::Worker::spawn`) clones from the
  sealed image with `CLONEf_MT_WORKER`.  Code (CVs, op trees, pad templates),
  globs, stashes, and compiled regex programs are shared read-only.  Mutable
  state lives in per-worker side tables:
  - `xcv_mt_id`-indexed CV depth and padlists (materialised on first call).
  - `gp_mt_id`-indexed GP indirection for worker-private `our` variables and
    glob assignments.
  - Per-worker hash/stash overlays in `hv_common` so runtime `eval` and
    `require` inside a worker define or redefine subroutines without touching
    the sealed image.
  - Per-worker MRO metadata, overload tables (`PL_mt_hv_amagic`), and regex
    heads.
- **Capabilities (`MT::Cap`)**: Cross-worker immutable values stored in a
  process-wide `krt` shared segment with atomic handle counting, generation
  tracking, and owner-side quiescent reclamation (`MT::Cap::reap`).

---

## 4. Two-Tier XS Isolation

`Perl_rpp_invoke_xs` dispatches XSUBs across two tiers:

- **Tier 1 (In-Process)**: Modules built with `-DPERL_XS_THREADSAFE=1` and
  `PERL_NO_GET_CONTEXT` emit an exported `mt_threadsafe_boot_<Mod>` marker.
  Their XSUBs are stamped `CVf_MT_THREADSAFE` and execute directly in any
  worker thread using per-interpreter `MY_CXT` + `CLONE`.
- **Tier 2 (Out-of-Process XS Compartments)**: Modules without the marker,
  loaded while workers are active, are routed to forked **XS compartments**
  (`PERL_MT_XS_COMPARTMENTS`, default 2).  Communication runs over a shared
  `krt` segment containing per-compartment MPSC request rings, per-slot SPSC
  reply rings, and bump arenas (`PERL_MT_XS_ARENA_MB`, default 8 MiB per slot).
  The TLV wire format supports scalars, deep-copied arrays/hashes, scalar
  write-backs, `$!` propagation, exception objects, host-to-compartment
  callbacks, remote code references (`MT::XS::RemoteCode`), tied blessed
  objects (`MT::XS::TiedHash`, `MT::XS::TiedArray`), and explicit proxy access
  (`MT::XS::remote`).  Compartment crashes bump the generation, fail pending
  calls with `MT_COMPARTMENT_DEAD`, and invalidate stale handles
  (`MT_STALE_REMOTE_HANDLE`).

---

## 5. Stackful Fibers, Adaptive Waiting & `:async` I/O

- **Context Switch (`mt_ctx_*.c`)**: Self-gated per-architecture assembly/ABI
  implementations for `x86_64` (`mt_ctx_amd64.c`), Linux `ppc64le` ELFv2
  (`mt_ctx_ppc64le.c`), `arm64` AAPCS64 (`mt_ctx_arm64.c`), 64-bit AIX and
  IBM i PASE XCOFF/TOC (`mt_ctx_ppc64_aix.c`), and `s390x` ELF + z/OS USS
  LP64/XPLINK-64 (`mt_ctx_s390x.c`).
- **Fiber State (`mt_fiber.h`, `mt_fiber.c`)**: Each fiber runs on a guarded
  stacklet (`MT_FIBER_STACK`, default 64 KiB, LIFO-recycled on join) and swaps
  the explicit `MT_VMSTATE_FIELDS` set (Perl stacks, `PL_curcop`, `PL_op`,
  `PL_curpad`, `PL_top_env`, match state, stat state, sort state, and errors)
  while keeping per-worker tables, `%ENV`, and the XS compartment slot shared
  across fibers on the same worker.
- **Scheduler & `krt_vec`**: One cooperative loop per interpreter with
  `PERL_ASYNC_CHECK` tick preemption, deadlock detection, and cache-line-aligned
  `krt_vec` waiters (`umwait`, `ldaxr`/`wfe`, POWER low-priority spin, falling
  back to futex/ulock/condvar with adaptive spin budgets).
- **`:async` PerlIO & `ext/MT`**: Non-blocking file/socket operations via
  `krt` AIO rings (`io_uring` on Linux, libdispatch+kqueue on macOS,
  pollset/poll worker pools on AIX/PASE/z/OS) that park only the calling fiber.
  Higher-level primitives (`MT::Pool`, `MT::Channel`, `MT::Cell`,
  `MT::SharedHash`, `MT::IO`) build on these layers.

---

## 6. Fault Tolerance, Supervision & Partitioning

- **Fail-Fast Fault Model (`mt_fault.c`, `MT::Fault`)**: Classifies faults into
  recoverable (stops the offending worker or compartment, records a structured
  event with sequence number, cause, unit kind/id, and message) and fatal
  (immediate process termination).  Observers register via `MT::Fault::on(\&cb)`
  and drain events via `MT::Fault::poll()`.
- **Heartbeat Supervision (`MT::Supervisor`)**: Workers and compartments emit
  periodic heartbeats into a `krt_vec` liveness table (`PERL_MT_HEARTBEAT_MS`,
  default 100 ms).  `MT::Supervisor::tick()` scans one pending-bit word per
  call and faults units after `PERL_MT_HEARTBEAT_MISS` (default 3) missed beats.
- **Partitioning Architecture**: The runtime also provides checkpointing,
  process pairs, cross-process shared boot images, isolated namespaces, and
  LPAR/WPAR/WLM-style workload classes (`MT::Checkpoint`, `MT::SharedImage`,
  `MT::Namespace`, `MT::Class`, `MT::Partition`).

---

## 7. Data Structures & SIMD Kernels

- **SIMD Dispatch (`simd/perl_simd_dispatch.h`, `simd/perl_simd.c`)**:
  `Perl_simd_detect()` runs once at interpreter construction.  `PL_simd_level`
  holds the baseline tier (`SCALAR`, `SSE2`, `AVX2`, `VSX`, `NEON`) while
  `Perl_simd_extended_level()` and `Perl_simd_caps()` expose extended CPU ISA
  tiers (`AVX-512`, `AVX2-VNNI`, `AVX-512-VNNI`, `VSX2`, `VSX3`, `s390x VX`,
  `s390x NNPA`, `NEON dotprod/i8mm`, `Apple AMX`).  Kernels use portable
  GCC/Clang vector extensions, enforce a 64-byte minimum length threshold
  (`PERL_SIMD_MIN_LEN`), never over-read past `[s, s+len)`, and are disabled
  automatically on EBCDIC builds or under `PERL_FORCE_SCALAR=1`.
- **String & Regex Kernels**:
  - ASCII invariant span & high-byte count (`perl_simd_bytes.h`).
  - UTF-8 RFC 3629 classification in 1024-byte windows (`perl_simd_utf8.h`).
  - Single-range `tr///` and ASCII `uc`/`lc` (`perl_simd_bytes.h`).
  - Regex `EXACT`, `ANYOFM`/`NANYOFM`, bitmap `ANYOF`, and single-range `ANYOFR`
    span/search kernels (`perl_simd_regex.h`).
- **SwissTable `HV` (`hv.c`, `hv.h`)**: Open-addressed control-byte probe
  groups behind Perl's hash interface, preserving `Hash::Util`, `keys`/`each`,
  `Storable`, and tie semantics.
- **Packed Typed Arrays (`av.c`, `av.h`, `MT::Array`)**: Opt-in contiguous
  native `IV`, `NV`, and `U8` buffers stored behind standard `AV`s with
  correctly-rounded reductions (`sum`, `min`, `max`, `dot`, `map_add`, `slice`,
  `count`, `find`).  Any raw `AvARRAY` consumer unpacks the array back to
  standard `SV*` slots.

---

## 8. Deterministic Numerics & Extensible Acceleration

### Deterministic Numerics (`core-math/`, `ext/MT-Decimal/`)
- **Contraction Control**: `cflags.SH` probes and adds `-ffp-contract=off` so
  FMA contraction cannot diverge across x86-64, ARM64, POWER, and z/Architecture.
- **Exact Parsing & Shortest Formatting**: `Perl_my_atof3` uses Eisel-Lemire
  with an exact arbitrary-precision fallback (`core-math/mt_parse_nv.h`).
  Opt-in Ryu shortest round-trip formatting (`core-math/mt_ryu.h`) activates
  under `PERL_NV_SHORTEST=1`.
- **Correctly-Rounded `core-math`**: Self-contained double-double Ziv fast
  paths with a 256-bit deterministic software fallback and Payne-Hanek range
  reduction (`core-math/cr_libm.h`, `core-math/cr_fp256.h`) for `sin`, `cos`,
  `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `log10`, `pow`, `sqrt`,
  and `fmod`.
- **NaN Canonicalisation**: `core-math/mt_nan.h` normalises all NaN outputs at
  arithmetic op and conversion boundaries to `0x7ff8000000000000`.
- **`MT::Decimal`**: Thread-safe XS module providing 34-digit IEEE 754-2008
  decimal128 arithmetic, operator overloading, all five rounding modes,
  `quantize`, and canonical 16-byte BID128 binary serialization.

### Extensible Offload Framework (`accel/`)
`accel/perl_accel.h` defines a `perl_accel_backend_t` vtable and priority
registry with runtime hardware probing, element-count thresholds
(`PERL_ACCEL_THRESHOLD`), and environment selection
(`PERL_ACCEL=auto|off|<backend>`).

| Backend | Device Class | Notes |
|---|---|---|
| `scalar` | CPU | Always-available exact reference twin (Shewchuk/exact CR reductions) |
| `accelerate` | CPU | macOS Accelerate (`vDSP`/BLAS) with Ozaki correctly-rounded `sum_f64_cr`/`dot_f64_cr` |
| `metal` | GPU | Apple Metal compute dispatch on macOS |
| `ane` | NPU | Apple Silicon Neural Engine probe/dispatch |
| `mesa_opencl` | GPU | Mesa OpenCL with strict AMD & Intel GPU vendor filter |
| `intel_npu` | NPU | Intel NPU / Level Zero runtime probe |
| `amd_npu` | NPU | AMD XDNA / XRT NPU runtime probe |
| `power_nx` | Coproc | IBM POWER NX compression/offload coprocessor |
| `s390x_nnpa` | Coproc | IBM z16 / Telum II NNPA facility |

Every offload backend must match `accel_scalar.c` bit-for-bit on CR reductions
and automatically falls back to the scalar twin below threshold or on any
runtime error.

---

## 9. Core VM Optimizations

- **RC-Stack PADTMP Elimination**: Reduces mortalisation churn under
  `PERL_RC_STACK` in `pp_entersub` and marks `MT::Image` XSUBs `CvXS_RCSTACK_on`.
- **Call/Return Fast Paths**: Shared `S_entersub_cv` with unrolled
  small-argument copy, fused `OP_NEXTSTATE + OP_ARGCHECK` signature dispatch,
  XSUB lvalue-mask fast check, and immortal scalar return fast path in
  `pp_leavesub`.
- **Peephole Superinstructions**: Fuses compare-and-branch (`eq`/`ne`/
  `lt`/`gt`/`le`/`ge`/`defined` + `cond_expr`), `shift` + `padsv_store`, and
  `method_named` + `entersub` in `op.c`/`pp_hot.c` while keeping `op_type` and
  `op_next` intact for full `B::Deparse` compatibility.
- **Overlay-Validated Inline Caches**: Caches frozen method resolution,
  constant-key hash reads, and worker glob access, invalidated automatically on
  per-worker shadow, GP, MRO, or `sub_generation` mutations.
- **Freeze-Time Specialisation**: Folds pure constant frozen subroutines
  and devirtualises static class method calls during `perl_freeze_boot_image`,
  guarded by overlay validation and `PERL_MT_NO_SPECIALIZE=1`.

---

## 10. Platform Portability Layer (`krt/` & `mt_ctx_*.c`)

| Platform | `krt/` Backend | Fiber Context Switch | Notes |
|---|---|---|---|
| Linux (`x86_64`, `ppc64le`, `arm64`, `s390x`) | `krt_linux.c` | `mt_ctx_amd64.c`, `mt_ctx_ppc64le.c`, `mt_ctx_arm64.c`, `mt_ctx_s390x.c` | `memfd_create` + pkey sealing, `futex`, `rseq`, `io_uring` + `eventfd` |
| macOS (`arm64`, `x86_64`) | `krt_darwin.c` | `mt_ctx_arm64.c`, `mt_ctx_amd64.c` | `mach_vm_map`/`mach_vm_protect`, `__ulock_wait2`, libdispatch + `kqueue` |
| AIX 7.x (`ppc64`) | `krt_aix.c` | `mt_ctx_ppc64_aix.c` | SysV `shmget`/`shmat` + `mprotect`, XCOFF/TOC descriptors, `pollset` AIO pool |
| IBM i PASE (`ppc64`) | `krt_aix.c` | `mt_ctx_ppc64_aix.c` | Runtime PASE detection (`/QOpenSys/usr/lib/libpase.a`), POSIX `poll` AIO pool |
| z/OS USS (`s390x`, ASCII & EBCDIC) | `krt_zos.c` | `mt_ctx_s390x.c` | Standard LP64 & XPLINK-64 (`swapcontext`), Enhanced ASCII (`-qascii`) & IBM-1047 EBCDIC |

---

## 11. Environment Variable Quick Reference

| Variable | Default | Purpose |
|---|---|---|
| `PERL_MT_IMAGE` | unset | Set `1` to allocate op slabs in the arena and auto-freeze after `INIT` |
| `PERL_MT_ARENA_MB` | `512` | Boot image arena size in MiB |
| `PERL_MT_CAP_SLOTS` | `65536` | Process-wide capability table slots |
| `PERL_MT_XS_COMPARTMENTS` | `2` | Out-of-process XS compartment count (`0` disables Tier 2) |
| `PERL_MT_XS_SLOTS` | `64` | Concurrent bridge slots |
| `PERL_MT_XS_ARENA_MB` | `8` | Per-slot bridge payload arena in MiB |
| `PERL_MT_HEARTBEAT_MS` | `100` | Worker/compartment liveness heartbeat interval (ms) |
| `PERL_MT_HEARTBEAT_MISS` | `3` | Missed heartbeats before `MT::Supervisor` faults a unit |
| `MT_FIBER_STACK` | `65536` | Fiber stacklet size in bytes (minimum 16384) |
| `MT_WORKER_STACK` | `8388608` | Worker pthread stack size in bytes |
| `PERL_FORCE_SCALAR` | unset | Set `1` to force scalar twins across all SIMD and accel paths |
| `PERL_SIMD_LEVEL` | auto | Cap SIMD tier (`scalar`, `narrow`, or specific tier name) |
| `PERL_ACCEL` | `auto` | Offload backend selection (`auto`, `off`, or backend name) |
| `PERL_ACCEL_THRESHOLD` | `4096` | Minimum element count for offload dispatch |
| `PERL_NV_SHORTEST` | unset | Set `1` to enable Ryu shortest round-trip NV formatting |
| `PERL_MT_NO_SPECIALIZE` | unset | Set `1` to disable freeze-time specialisation |
| `PERL_MT_DEBUG_IMMORTAL` | unset | Set `1` to enable extra immortal mutation checks |
| `PERL_MT_XS_DEBUG` | unset | Set `1` to trace XS compartment loads, hooks, and fallbacks |
| `PERL_MT_LONG_TESTS` | unset | Set `1` to enable extended stress cases in `t/mt/` |

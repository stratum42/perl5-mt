# perl5-mt Reference Manual

`perl5-mt` is a fork of Perl 5.40.2 that adds shared-memory worker threads over
a sealed boot image, cooperative stackful fibers, two-tier XS isolation,
kernel-async I/O, SwissTable hashes, packed typed arrays, dual-gated SIMD and
hardware offload kernels, cross-host deterministic numerics, and VM call
optimizations.  Existing Perl 5.40.2 programs and CPAN modules continue to run
unchanged.

For architecture and wire-protocol details, see [MT_DESIGN.md](MT_DESIGN.md).
For reproducible measurements and cross-host numeric digests, see
[MT_BENCH.md](MT_BENCH.md).  For contribution guidelines and platform testing,
see [CONTRIBUTING.md](CONTRIBUTING.md).

---

## 1. Building, PGO & Packaging

### Linux (`x86_64`, `ppc64le`, `arm64`, `s390x`)

GCC 15+ defaults to C23, so pass `-std=gnu17`.  Linux builds require
`-Ud_thread_local` so `libperl` and dynamically loaded extensions do not use
static TLS (`__thread` / `_Thread_local`), which breaks `dlopen` under many
worker threads.

```sh
./Configure -des -Dusethreads -Ud_thread_local \
    -Dprefix=$HOME/localperl \
    -Accflags='-std=gnu17' -Doptimize='-O2 -g'
make -j$(nproc)
TEST_JOBS=$(nproc) make test_harness
make install
```

### macOS (`arm64`, `x86_64`)

```sh
./Configure -des -Dusethreads \
    -Dprefix=$HOME/localperl -Doptimize='-O2 -g'
make -j$(sysctl -n hw.ncpu)
TEST_JOBS=$(sysctl -n hw.ncpu) make test_harness
make install
```

### Build Optimization Flags

`Configure`, `cflags.SH`, and `Makefile.SH` support four optional tuning flags:

- `-Dusepgo`: two-stage Profile-Guided Optimization via `make pgo`, trained by
  `tools/pgo-train.pl` (Clang `llvm-profdata` or GCC `.gcda`, with optional
  Linux BOLT and macOS `ld64` order-file hooks).
- `-Dusenative`: host-native CPU tuning (`-march=native` / `-mcpu=native`),
  preserving `-ffp-contract=off` and excluding `-ffast-math`.
- `-Dusesymbolbinding`: ELF symbolic function binding
  (`-Wl,-Bsymbolic-functions`).
- `-Dusehugepagetext`: 2 MiB hugepage-aligned text segment layout on Linux.

Example PGO + native build on Linux (drop `-Ud_thread_local` and
`-Accflags='-std=gnu17'` on macOS):

```sh
./Configure -des -Dusethreads -Ud_thread_local \
    -Dprefix=$HOME/localperl -Accflags='-std=gnu17' \
    -Dusepgo -Dusenative
make -j$(nproc) pgo
```

### Packaging & Test Suites

Packaging definitions live under `pkg/`:

- RPM: `pkg/rpm/perl5-mt.spec` (`--with pgo`, `--with native`, installs under
  `/opt/perl5-mt`).
- Debian: `pkg/debian/control`, `pkg/debian/rules`, `pkg/debian/changelog`.
- macOS: `pkg/macos/build-pkg.sh` (`--pgo`, `--native`, builds a `.pkg`).

Verification suites:

```sh
make -C krt test                  # kernel runtime (also SAN=thread, SAN=address)
make -C accel test                # offload registry, CR parity, SIMD ISA probes
./perl -I. -Ilib -MTest::Harness -e 'runtests(@ARGV)' t/mt/*.t
make test_porting                 # MANIFEST, exec-bit, and porting checks
```

Set `PERL_MT_LONG_TESTS=1` to enable extended stress cases in `t/mt/`.

---

## 2. Boot Image, Workers, Fibers & Async I/O

### Sealed Boot Image & Workers

A program compiles and runs its `BEGIN` and `INIT` blocks as usual.  Calling
`MT::Image::freeze()` (or setting `PERL_MT_IMAGE=1`, which allocates op slabs
in the arena and freezes automatically after `INIT`) clones the interpreter
into a sealable `perl_krt_shm_t` arena (`PERL_MT_ARENA_MB`, default 512 MiB).
Every SV in the frozen clone is marked `SVf_IMMORTAL | SVf_READONLY | SVf_PROTECT`
with a saturated reference count, and the arena is sealed read-only via Linux
memory protection keys (`pkey_mprotect`, re-armed `NOWRITE` at every
`JMPENV_PUSH` landing) or `mprotect` / `mach_vm_protect`.  A stray write traps
at the offending instruction (`SEGV_PKUERR` / `SIGSEGV`) or croaks
`MT_IMMORTAL_MUTATION`.

`MT::Worker::spawn('Pkg::sub', @args)` and `MT::Worker::join($id)` run fresh
worker threads cloned from the sealed image (`CLONEf_MT_WORKER`).  Code (CVs,
op trees, pad templates), globs, stashes, and compiled regex programs are
shared read-only; each worker maintains private side tables for CV padlists and
depths, GP indirection (`our` variables), stash/hash overlays (allowing runtime
`eval` and `require` inside workers), MRO metadata, and overload tables.
Worker pthreads use an 8 MiB stack by default (`MT_WORKER_STACK`).

`MT::Cap` publishes immutable cross-worker values in a process-wide shared
segment (`PERL_MT_CAP_SLOTS`, default 65536) with atomic handle counting,
generation checks, and owner-side reclamation (`MT::Cap::reap`).

### Channels, Cells, Shared Hashes & Worker Pools

Core concurrency primitives live in `mt_chan.c` and are wrapped by `ext/MT`:

- `MT::Channel`: bounded/unbounded typed value queues.  Values cross as copies
  via `Perl_mt_value_from_sv` / `Perl_mt_value_to_sv` (scalars, capabilities,
  and shared memory pass; mutable references croak `MT_UNSHAREABLE_REF`).
  Cross-worker wakes post directly to the receiver's `krt_vec` slot vector for
  immediate wakeups.
- `MT::Cell`: atomic shared scalar cell with lock-free compare-and-swap (`cas`)
  on body pointers and hazard-pointer reclamation of retired bodies.
- `MT::SharedHash`: sharded concurrent hash table (64 buckets per shard) with
  atomic per-key operations.
- `MT::Pool`: supervised worker pool (`new`, `map`, `submit`, `shutdown`,
  `named`, `faults`) supporting nested data and capability marshaling,
  fault-triggered respawn with exponential backoff, and optional primary/backup
  process pairs (`pairs => 1`).  At interpreter exit, `MT::Pool`'s `END` block
  shuts down live pools, and `Perl_mt_worker_join_all` joins any remaining
  workers before mutex teardown (warning if any were still running).

### Stackful Fibers & Scheduler

`MT::Fiber` provides asymmetric stackful fibers (`spawn`, `resume`, `yield`,
`join`, `stats`) and a per-interpreter cooperative scheduler (`schedule`, `run`,
`park`, `pass`, `ready`).

- Each fiber runs on a guarded stacklet (`MT_FIBER_STACK`, default 64 KiB,
  minimum 16 KiB, with one guard page below it) that is LIFO-recycled on
  `join`.
- Switching swaps the explicit `MT_VMSTATE_FIELDS` set (`mt_fiber.h`: Perl
  stacks, `PL_curcop`, `PL_op`, `PL_curpad`, `PL_top_env`, match state, stat
  state, sort state, and errors) while keeping per-worker tables, `%ENV`, and
  the XS compartment slot shared across fibers on the same worker.
- Context switches are implemented per architecture in `mt_ctx_amd64.c`,
  `mt_ctx_arm64.c`, `mt_ctx_ppc64le.c`, `mt_ctx_ppc64_aix.c`, and
  `mt_ctx_s390x.c`.  `MT::Ctx::selftest($n)` verifies callee-saved register
  and FP preservation.
- The scheduler runs one loop per interpreter, supports `PERL_ASYNC_CHECK` tick
  preemption, detects deadlocks (`MT_FIBER_DEADLOCK`), and integrates XS
  compartment replies, `:async` I/O completions, and channel waits through its
  poll and idle hooks (`Perl_mt_sched_poll`, `Perl_mt_sched_idle`).

### `:async` PerlIO Layer

`:async` (`perlio_async.c`) is a buffered (64 KiB) PerlIO layer backed by the
`krt` AIO ring (`io_uring` on Linux, libdispatch + `kqueue` on macOS, `pollset`
or `poll` pools on AIX / IBM i PASE / z/OS).  It supports `open`, `read`,
`write`, `seek`, `tell`, `close`, `MT::IO::stat` (`STATX`), `MT::IO::fsync`,
and `MT::IO::splice`.  Each ring attempts to register 32 fixed 64 KiB buffers
(`PERL_MT_AIO_BUFS`), falling back to unregistered buffers if `RLIMIT_MEMLOCK`
returns `ENOMEM` or `EPERM`.

Inside a scheduled fiber, blocking `:async` operations park only the calling
fiber.  On Linux 6.7+, when both `:async` I/O and XS compartment replies are
pending, the idle hook uses `IORING_OP_FUTEX_WAIT` to sleep on both sources in
a single `io_uring_enter` call; on older kernels and other OSes it falls back
to a bounded 1 ms vector wait.

---

## 3. Two-Tier XS Isolation

`Perl_rpp_invoke_xs` dispatches XSUB calls across two tiers:

1. **Tier 1 (In-Process)**: XS modules compiled with `-DPERL_XS_THREADSAFE=1`
   and `PERL_NO_GET_CONTEXT` export a `mt_threadsafe_boot_<Mod>` symbol.  Their
   XSUBs receive `CVf_MT_THREADSAFE` and execute directly in any worker thread,
   keeping mutable state in `MY_CXT` with a `CLONE` callback.
2. **Tier 2 (Out-of-Process XS Compartments)**: Legacy XS modules without the
   marker are routed to forked compartment processes (`PERL_MT_XS_COMPARTMENTS`,
   default 2) over a shared `krt` memory segment containing per-compartment
   MPSC request rings, per-slot SPSC reply rings, bump arenas
   (`PERL_MT_XS_ARENA_MB`, default 8 MiB), and a `krt_vec` doorbell table.

The TLV bridge protocol (`mt_xs_bridge.c`, `mt_xs_compartment.c`) supports
scalars, deep-copied arrays and hashes, scalar write-backs, `$!` propagation,
exception objects, host callbacks, remote coderefs (`MT::XS::RemoteCode`),
tied blessed objects (`MT::XS::TiedHash`, `MT::XS::TiedArray`), and explicit
proxies (`MT::XS::remote`).  Inside a scheduled fiber, a remote XS call parks
the fiber while the worker runs other fibers (capped at `PERL_MT_XS_INFLIGHT`,
default 64).  A reaper thread per compartment blocks in `waitpid`; if a
compartment crashes, pending callers wake immediately with
`MT_COMPARTMENT_DEAD`, the generation increments, and stale handles fail with
`MT_STALE_REMOTE_HANDLE`.

---

## 4. Data Structures, SIMD & Hardware Acceleration

Every SIMD kernel, container fast path, and offload backend carries an exact
scalar twin and a runtime kill-switch (`PERL_FORCE_SCALAR=1`, `PERL_ACCEL=off`,
compile-time `-DPERL_FORCE_SCALAR`).

- **SIMD String, UTF-8 & Regex Kernels (`simd/`)**: Runtime detection in
  `Perl_simd_detect()` selects baseline tiers (`scalar`, `sse2`, `avx2`, `vsx`,
  `neon`) and exposes extended ISA capabilities (`AVX-512`, `AVX2-VNNI`,
  `AVX-512-VNNI`, `VSX2`, `VSX3`, `s390x VX`, `s390x NNPA`, `NEON dotprod/i8mm`,
  `Apple AMX`).  Kernels enforce a 64-byte minimum length (`PERL_SIMD_MIN_LEN`),
  never over-read past `[s, s+len)`, and disable automatically on EBCDIC builds.
  Hooked operations include ASCII span/upgrade, RFC 3629 UTF-8 validation in
  1024-byte windows, single-range `tr///`, ASCII byte `uc`/`lc`, and regex
  `EXACT`, `ANYOFM`/`NANYOFM`, bitmap `ANYOF`, and range `ANYOFR` spans and
  searches.
- **SwissTable `HV` (`hv.c`, `hv.h`)**: Open-addressed control-byte probe
  groups behind Perl's hash API, preserving `Hash::Util`, `keys`/`each`,
  `Storable`, and tied hashes.  Toggle with `PERL_MT_SWISS=0`.
- **Packed Typed Arrays (`av.c`, `av.h`, `MT::Array`)**: Opt-in contiguous
  `IV`, `NV`, and `U8` arrays (`MT::Array::packed`, `MT::Array::pack`) with
  correctly-rounded reductions (`sum`, `min`, `max`, `dot`, `map_add`, `slice`,
  `count`, `find`) and transparent unpacking for any raw `AvARRAY` consumer.
- **Offload Backend Registry (`accel/`)**: Priority-ordered hardware dispatch
  (`scalar`, macOS `accelerate` with Ozaki correctly-rounded `sum_f64_cr` /
  `dot_f64_cr`, `metal`, `ane`, `mesa_opencl` restricted to AMD/Intel GPUs,
  `intel_npu`, `amd_npu`, `power_nx`, `s390x_nnpa`), controlled by `PERL_ACCEL`
  and `PERL_ACCEL_THRESHOLD`.  All backends must match `accel_scalar.c`
  bit-for-bit on correctly-rounded reductions.

---

## 5. Deterministic Numerics

`perl5-mt` enforces cross-host bit-identical IEEE 754 binary64 and decimal128
arithmetic across architectures and compilers (`t/mt/num_digest.t`):

- **Contraction Control**: `cflags.SH` adds `-ffp-contract=off` and forbids
  `-ffast-math`.
- **Exact Parsing & Opt-In Ryu**: `Perl_my_atof3` uses Eisel-Lemire with an
  exact arbitrary-precision fallback (`core-math/mt_parse_nv.h`).  Default
  `%.15g` formatting is unchanged; set `PERL_NV_SHORTEST=1` for Ryu shortest
  round-trip formatting (`core-math/mt_ryu.h`).
- **Correctly-Rounded `core-math`**: Vendored double-double Ziv fast paths with
  256-bit software fallback and Payne-Hanek range reduction
  (`core-math/cr_libm.h`, `core-math/cr_fp256.h`) for `sin`, `cos`, `tan`,
  `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `log10`, `pow`, `sqrt`, and
  `fmod`.
- **NaN Canonicalisation**: `core-math/mt_nan.h` normalises NaN results to
  quiet positive NaN (`0x7ff8000000000000`) at arithmetic and conversion
  boundaries.
- **`MT::Decimal` (`ext/MT-Decimal/`)**: Thread-safe XS module for 34-digit
  IEEE 754-2008 decimal128 arithmetic, operator overloading, all five rounding
  modes, `quantize`, and canonical 16-byte BID128 serialization.

---

## 6. Core VM Optimizations

Verified by `t/mt/vm_opt.t` both normally and under `PERL_MT_NO_SPECIALIZE=1`:

- **RC-Stack PADTMP Elimination (`pp_hot.c`)**: Avoids unnecessary PADTMP
  mortalisation under `PERL_RC_STACK` in `pp_entersub` and marks `MT::Image`
  XSUBs `CvXS_RCSTACK_on`.
- **Call/Return Fast Paths (`pp_hot.c`)**: Shared `S_entersub_cv` with unrolled
  small-argument copy, fused `OP_NEXTSTATE + OP_ARGCHECK` signature dispatch,
  XSUB lvalue-mask fast check, and immortal scalar return fast path in
  `pp_leavesub`.
- **Peephole Superinstructions (`op.c`, `pp_hot.c`)**: Fuses compare-and-branch
  (`eq`/`ne`/`lt`/`gt`/`le`/`ge`/`defined` + `cond_expr`), `shift` +
  `padsv_store`, and `method_named` + `entersub` while keeping `op_type` and
  `op_next` intact for `B::Deparse`.
- **Overlay-Validated Inline Caches (`mt_image.h`, `pp_hot.c`)**: Caches frozen
  method lookup, constant-key hash reads, and worker glob access, invalidating
  on per-worker shadow, GP, MRO, or `sub_generation` writes.
- **Freeze-Time Specialisation (`mt_freeze.c`)**: Folds pure constant frozen
  subroutines and devirtualises static class methods at freeze time, guarded by
  overlay checks and `PERL_MT_NO_SPECIALIZE=1`.

---

## 7. Fault Model, Supervision & Partitioning

- **Fail-Fast Fault Model (`mt_fault.c`, `MT::Fault`)**: Separates recoverable
  faults (stops the offending worker or compartment and appends a structured
  event) from fatal faults (terminates the process).  Observers subscribe via
  `MT::Fault::on(\&cb)` and drain via `MT::Fault::poll()` / `MT::Fault::last()`.
- **Heartbeat Supervision (`MT::Supervisor`)**: Workers and compartments post
  heartbeats to a `krt_vec` liveness table every `PERL_MT_HEARTBEAT_MS`
  (default 100 ms); `MT::Supervisor::tick()` faults units after
  `PERL_MT_HEARTBEAT_MISS` (default 3) missed beats.
- **Partitioning Modules**: `MT::Checkpoint`, `MT::SharedImage`,
  `MT::Namespace`, `MT::Class`, and `MT::Partition` support checkpoints,
  primary/backup process pairs, cross-process shared boot images, isolated
  namespaces, and workload classes.

---

## 8. Platform Support & `krt/`

`krt/` is a self-contained C11 kernel runtime adapter with no Perl header
dependencies (`krt/krt.h`, `krt/krt_ring.h`, `krt/krt_vec.h`, `krt/krt_wait.h`).
Each backend is a self-gated translation unit:

| Platform | `krt/` Backend | Fiber Context Switch | Primitives |
|---|---|---|---|
| Linux (`x86_64`, `ppc64le`, `arm64`, `s390x`) | `krt_linux.c` | `mt_ctx_amd64.c`, `mt_ctx_ppc64le.c`, `mt_ctx_arm64.c`, `mt_ctx_s390x.c` | `memfd_create` + pkey sealing, `futex`, `rseq`, `io_uring` + `eventfd` |
| macOS (`arm64`, `x86_64`) | `krt_darwin.c` | `mt_ctx_arm64.c`, `mt_ctx_amd64.c` | `mach_vm_map`/`mach_vm_protect`, `__ulock_wait2`, libdispatch + `kqueue` |
| AIX 7.x (`ppc64`) | `krt_aix.c` | `mt_ctx_ppc64_aix.c` | SysV `shmget`/`shmat` + `mprotect`, XCOFF/TOC, `pollset` AIO pool |
| IBM i PASE (`ppc64`) | `krt_aix.c` | `mt_ctx_ppc64_aix.c` | Runtime PASE detection (`/QOpenSys/usr/lib/libpase.a`), POSIX `poll` AIO pool |
| z/OS USS (`s390x`, ASCII & EBCDIC) | `krt_zos.c` | `mt_ctx_s390x.c` | LP64 and XPLINK-64 (`swapcontext`), Enhanced ASCII (`-qascii`) and IBM-1047 |
| Other platforms | `krt_stubs.c` | n/a | Compiles and returns `PERL_KRT_ENOSYS` |

---

## 9. Environment Variables, Error Codes & Known Limits

### Environment Variables

| Variable | Default | Purpose |
|---|---|---|
| `PERL_MT_IMAGE` | unset | Set `1` to allocate op slabs in the arena and auto-freeze after `INIT` |
| `PERL_MT_ARENA_MB` | `512` | Boot image arena size in MiB |
| `PERL_MT_CAP_SLOTS` | `65536` | Process-wide capability table slots |
| `PERL_MT_XS_COMPARTMENTS` | `2` | Out-of-process XS compartment count (`0` disables Tier 2) |
| `PERL_MT_XS_SLOTS` | `64` | Concurrent bridge slots |
| `PERL_MT_XS_ARENA_MB` | `8` | Per-slot bridge payload arena in MiB |
| `PERL_MT_XS_INFLIGHT` | `64` | Max in-flight remote XS calls per worker |
| `PERL_MT_XS_BATCH` | `32` | Requests served before liveness check in compartment |
| `PERL_MT_HEARTBEAT_MS` | `100` | Worker/compartment liveness heartbeat interval (ms) |
| `PERL_MT_HEARTBEAT_MISS` | `3` | Missed heartbeats before `MT::Supervisor` faults a unit |
| `MT_FIBER_STACK` | `65536` | Fiber stacklet size in bytes (minimum `16384`) |
| `MT_WORKER_STACK` | `8388608` | Worker pthread stack size in bytes |
| `PERL_KRT_POLL_NS` | auto | Adaptive vector wait spin budget override (ns) |
| `PERL_FORCE_SCALAR` | unset | Set `1` to force scalar reference twins across SIMD and `accel/` |
| `PERL_SIMD_LEVEL` | auto | Cap SIMD tier (`scalar`, `narrow`, or specific tier name) |
| `PERL_MT_SWISS` | `1` | Set `0` to disable SwissTable `HV` probing |
| `PERL_ACCEL` | `auto` | Offload backend (`auto`, `off`, or backend name) |
| `PERL_ACCEL_THRESHOLD` | `4096` | Minimum element count for offload dispatch |
| `PERL_NV_SHORTEST` | unset | Set `1` to enable Ryu shortest round-trip NV formatting |
| `PERL_MT_NO_SPECIALIZE` | unset | Set `1` to disable freeze-time specialisation |
| `PERL_MT_DEBUG_IMMORTAL` | unset | Set `1` to enable extra immortal mutation checks |
| `PERL_MT_XS_DEBUG` | unset | Set `1` to trace XS compartment loads and calls |
| `PERL_MT_LONG_TESTS` | unset | Set `1` to enable extended stress cases in `t/mt/` |

### Common Error Codes

- Boot image & workers: `MT_IMMORTAL_MUTATION`, `MT_CLOSURE_NOT_SHARED`,
  `MT_UNSHAREABLE_REF`.
- XS compartments: `MT_COMPARTMENT_DEAD`, `MT_STALE_REMOTE_HANDLE`,
  `MT_XS_TOO_MANY_INFLIGHT`.
- Fibers & scheduler: `MT_NOT_A_FIBER`, `MT_NOT_IN_FIBER`, `MT_FIBER_FINISHED`,
  `MT_FIBER_BUSY`, `MT_YIELD_IN_COMPILE`, `MT_EXIT_IN_FIBER`, `MT_CTX_STACK`,
  `MT_SHSTK_UNSUPPORTED`, `MT_FIBER_NOT_SCHEDULED`, `MT_FIBER_IS_SCHEDULED`,
  `MT_FIBER_NOT_FINISHED`, `MT_FIBER_DEADLOCK`, `MT_SCHED_NESTED`.

### Known Limits

- `:async` handles do not support `accept`, `connect`, or `dup`, and
  `MT::IO::splice` flushes buffered data before operating on descriptors.
- Intel CET shadow stacks are not supported for fiber context switching
  (`MT::Ctx::selftest` croaks `MT_SHSTK_UNSUPPORTED` when active).
- Fiber yields and resumes are refused during compilation
  (`MT_YIELD_IN_COMPILE`).
- The Perl debugger (`-d`) is not supported after `MT::Image::freeze()`.
- `MT::SharedHash` uses 64 buckets per shard and does not resize dynamically;
  channel, cell, and shared-hash table slots persist for the lifetime of the
  process (retired `MT::Cell` value bodies are reclaimed via hazard pointers).
- A worker thread that never returns blocks process exit during
  `Perl_mt_worker_join_all`.
- `win32/` and `vms/` makefiles do not build the `mt_*` runtime files.

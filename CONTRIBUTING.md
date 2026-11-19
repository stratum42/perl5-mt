# Contributing to perl5-mt

Patches, bug reports, platform test logs, and benchmark runs are all welcome.
`perl5-mt` builds on upstream Perl 5.40.2 and keeps the same dual Artistic
License / GNU GPL licensing (see `Artistic` and `Copying`).

---

## 1. Building & Running the Test Suites

### Linux (`x86_64`, `ppc64le`, `arm64`, `s390x`)

```sh
./Configure -des -Dusethreads -Ud_thread_local \
    -Dprefix=$HOME/perl5-mt-inst \
    -Accflags='-std=gnu17' -Doptimize='-O2 -g'
make -j$(nproc)
```

### macOS (`arm64`, `x86_64`)

```sh
./Configure -des -Dusethreads \
    -Dprefix=$HOME/perl5-mt-inst \
    -Doptimize='-O2 -g'
make -j$(sysctl -n hw.ncpu)
```

### Verification Suites

Before opening a pull request, run the suites that cover your changes:

```sh
# Standalone kernel runtime adapter (also try SAN=thread and SAN=address)
make -C krt test

# Standalone acceleration registry, CR parity, and SIMD ISA probes
make -C accel test

# perl5-mt core tests (add PERL_MT_LONG_TESTS=1 for extended stress runs)
./perl -I. -Ilib -MTest::Harness -e 'runtests(@ARGV)' t/mt/*.t

# Full Perl core regression suite
TEST_JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu) make test_harness

# Porting, MANIFEST, and lint checks
make test_porting
```

---

## 2. Engineering Invariants

Every change must hold these lines:

- **Scalar Parity**: Anything touching `simd/`, `accel/`, `hv.c`, `av.c`, or
  `MT::Array` must match its scalar reference under `PERL_FORCE_SCALAR=1` and
  `PERL_ACCEL=off`.  Verify with `t/mt/simd_parity.t`, `t/mt/hv_swiss.t`,
  `t/mt/av_packed.t`, and `t/mt/accel.t`.
- **Cross-Host Numeric Determinism**: Never enable `-ffast-math`, and always
  preserve `-ffp-contract=off`.  Run `t/mt/num_digest.t` and verify that the
  SHA-256 numeric digest matches across platforms.
- **VM & Deparse Compatibility**: Changes to `op.c`, `pp_hot.c`, or
  `mt_freeze.c` must preserve `B::Deparse` output and pass `t/mt/vm_opt.t`
  both by default and with `PERL_MT_NO_SPECIALIZE=1`.
- **Thread-Safe XS**: New in-process XS extensions must define
  `PERL_NO_GET_CONTEXT`, compile with `-DPERL_XS_THREADSAFE=1`, keep mutable
  state in `MY_CXT`, and implement `CLONE`.
- **No Static TLS on Linux**: Do not introduce `__thread` or `_Thread_local`
  in `libperl` or loadable extensions; Linux builds require `-Ud_thread_local`.
- **Upstream Perl Compatibility**: Existing Perl 5.40.2 semantics and core
  tests must keep passing.

---

## 3. Platform Testing & Community Hardware Verification

Today, `perl5-mt` is regularly hardware-verified on `macOS arm64` (Apple
Silicon) and `Linux x86_64`, with earlier hardware runs on `Linux ppc64le`
(POWER8).  The tree also carries software-complete native kernel-runtime
(`krt/`), fiber context-switch (`mt_ctx_*.c`), and offload (`accel/`) backends
for:

- **AIX 7.x (`ppc64` XCOFF/TOC)**: `krt/krt_aix.c`, `mt_ctx_ppc64_aix.c`,
  `hints/aix.sh`
- **IBM i PASE (`ppc64`)**: `krt/krt_aix.c` (runtime PASE detection),
  `mt_ctx_ppc64_aix.c`, `hints/os400.sh`
- **z/OS USS (`s390x` LP64 & XPLINK-64, Enhanced ASCII & EBCDIC)**:
  `krt/krt_zos.c`, `mt_ctx_s390x.c`, `hints/os390.sh`
- **Linux `ppc64le`, `arm64`, `s390x`**, and **additional offload backends**
  (`Apple Metal`, `ANE`, `Mesa OpenCL` on AMD/Intel GPUs, `Intel NPU`,
  `AMD NPU`, `POWER NX`, `IBM z16/Telum II NNPA`)

If you have access to any of those machines, we would especially appreciate:

- Build logs plus `t/mt/platform_abi.t` and `make -C krt test` results
- Cross-host SHA-256 verification reports from `t/mt/num_digest.t`
- Compiler, linker, or ABI fixes for XL C/Clang, IBM Open XL, or z/OS
  `xlc`/`ibm-clang`
- Benchmark runs via `./perl -I. -Ilib tools/bench-suite.pl`

---

## 4. Submitting Changes

1. Keep commits focused, self-contained, and clearly explained.
2. Update `MANIFEST` and `Porting/exec-bit.txt` whenever you add files or
   executable scripts, and verify that `make test_porting` passes.
3. Add test coverage in `t/mt/`, `krt/`, or `accel/` for new behaviour or bug
   fixes.
4. For general Perl source hacking conventions, see `pod/perlhack.pod`.

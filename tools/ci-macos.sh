#!/bin/sh
# Run the perl5-mt CI steps on a remote arm64 macOS host over ssh.
# Usage:
#   tools/ci-macos.sh [--default|--pgo|--pgo-native|--all] [host] [remote-dir] [git-ref]
# Environment:
#   PERL_MT_MACOS_HOST=<ssh-host>           (required unless [host] is passed)
#   PERL_MT_MACOS_DIR=<remote-checkout>     (default: ~/Developer/perl5-mt-work/perl5-mt)
#   CI_MACOS_MODE=default|pgo|pgo-native|all (default: default)
#   CI_MACOS_LOG=<path>
set -eu

MODE=${CI_MACOS_MODE:-default}
while [ $# -gt 0 ]; do
  case "$1" in
    --default)    MODE=default; shift ;;
    --pgo)        MODE=pgo; shift ;;
    --pgo-native) MODE=pgo-native; shift ;;
    --all)        MODE=all; shift ;;
    --)           shift; break ;;
    -*)           echo "Unknown option: $1" >&2; exit 2 ;;
    *)            break ;;
  esac
done

HOST=${1:-${PERL_MT_MACOS_HOST:?set PERL_MT_MACOS_HOST or pass host as \$1}}
RDIR=${2:-${PERL_MT_MACOS_DIR:-'~/Developer/perl5-mt-work/perl5-mt'}}
REF=${3:-$(git rev-parse --abbrev-ref HEAD)}
LOG=${CI_MACOS_LOG:-ci-macos-${MODE}-$(date +%Y%m%d-%H%M%S).log}

run_formula() {
  formula=$1
  case "$formula" in
    default)
      cfg_extra="-Doptimize='-O2 -g'"
      build_target=""
      ;;
    pgo)
      cfg_extra="-Dusepgo"
      build_target="pgo"
      ;;
    pgo-native)
      cfg_extra="-Dusepgo -Dusenative"
      build_target="pgo"
      ;;
    *)
      echo "Unknown formula: $formula" >&2
      exit 2
      ;;
  esac

  echo "=== macOS CI formula: $formula (host=$HOST ref=$REF) ==="
  ssh -o BatchMode=yes "$HOST" "set -e; cd $RDIR;
    git fetch --all --tags -q;
    git checkout -q $REF;
    git reset -q --hard origin/$REF 2>/dev/null || true;
    [ -f Makefile ] && make -s distclean >/dev/null 2>&1 || true;
    ./Configure -des -Dusethreads -Dprefix=\$HOME/perl5-mt-inst $cfg_extra >configure.log 2>&1;
    make -j\$(sysctl -n hw.ncpu) $build_target >build.log 2>&1;
    if [ -f t/mt/build_flags.t ]; then ./perl -I. -Ilib t/mt/build_flags.t; fi;
    if [ -d krt ]; then make -C krt test; fi;
    PERL_MT_LONG_TESTS=1 TEST_JOBS=\$(sysctl -n hw.ncpu) make test_harness 2>&1 | tail -40;
    if [ -f t/mt/simd_parity.t ]; then
      PERL_FORCE_SCALAR=0 ./perl -I. -Ilib t/mt/simd_parity.t;
      PERL_FORCE_SCALAR=1 ./perl -I. -Ilib t/mt/simd_parity.t;
    fi;
    if [ -f t/mt/num_digest.t ]; then
      PERL_FORCE_SCALAR=0 ./perl -I. -Ilib t/mt/num_digest.t;
      PERL_FORCE_SCALAR=1 ./perl -I. -Ilib t/mt/num_digest.t;
    fi;
    if [ -x tools/bench/run.sh ] && [ -f tools/bench/mt.pl ]; then
      sh tools/bench/run.sh ./perl tools/bench/mt.pl 4 2000 5 \"\$PWD/lib\";
    fi;
    if [ -n '$build_target' ]; then make pgo-clean >/dev/null 2>&1 || true; fi"
}

case "$MODE" in
  all)
    {
      run_formula default
      run_formula pgo
      run_formula pgo-native
    } | tee "$LOG"
    ;;
  default|pgo|pgo-native)
    run_formula "$MODE" | tee "$LOG"
    ;;
  *)
    echo "Unknown CI_MACOS_MODE: $MODE" >&2
    exit 2
    ;;
esac

echo "macOS CI log: $LOG"

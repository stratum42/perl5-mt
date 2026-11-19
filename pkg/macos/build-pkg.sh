#!/bin/sh
# Build a macOS installer .pkg for perl5-mt using pkgbuild.
# Thin wrapper over Configure, make (or make pgo), make install, and pkgbuild.
#
# Usage:
#   pkg/macos/build-pkg.sh [options]
# Options:
#   --prefix=PATH      Install prefix (default: /opt/perl5-mt)
#   --version=VER      Package version (default: 5.40.2)
#   --pgo              Build with -Dusepgo and `make pgo`
#   --native           Build with -Dusenative
#   --output=FILE      Output .pkg path (default: perl5-mt-<ver>-<arch>.pkg)
#   --skip-tests       Skip post-build verification gates
#   --dry-run          Print commands without executing
set -eu

PREFIX=/opt/perl5-mt
VERSION=5.40.2
USE_PGO=0
USE_NATIVE=0
SKIP_TESTS=0
DRY_RUN=0
ARCH=$(uname -m)
OUTPUT=""

for arg in "$@"; do
    case "$arg" in
        --prefix=*)   PREFIX=${arg#--prefix=} ;;
        --version=*)  VERSION=${arg#--version=} ;;
        --pgo)        USE_PGO=1 ;;
        --native)     USE_NATIVE=1 ;;
        --output=*)   OUTPUT=${arg#--output=} ;;
        --skip-tests) SKIP_TESTS=1 ;;
        --dry-run)    DRY_RUN=1 ;;
        --help|-h)
            sed -n '2,15p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg" >&2
            exit 2
            ;;
    esac
done

if [ -z "$OUTPUT" ]; then
    OUTPUT="perl5-mt-${VERSION}-${ARCH}.pkg"
fi

NCPU=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
STAGE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/perl5-mt-pkg.XXXXXX")
trap 'rm -rf "$STAGE_DIR"' EXIT

CFG_ARGS="-des -Dusethreads -Dprefix=${PREFIX}"
BUILD_TARGET="all"
if [ "$USE_PGO" -eq 1 ]; then
    CFG_ARGS="$CFG_ARGS -Dusepgo"
    BUILD_TARGET="pgo"
else
    CFG_ARGS="$CFG_ARGS -Doptimize='-O2 -g'"
fi
if [ "$USE_NATIVE" -eq 1 ]; then
    CFG_ARGS="$CFG_ARGS -Dusenative"
fi

run_cmd() {
    echo "+ $*"
    if [ "$DRY_RUN" -eq 0 ]; then
        eval "$@"
    fi
}

if [ "$DRY_RUN" -eq 0 ] && ! command -v pkgbuild >/dev/null 2>&1; then
    echo "Error: pkgbuild not found (macOS Command Line Tools required)." >&2
    exit 1
fi

run_cmd "./Configure $CFG_ARGS"
run_cmd "make -j${NCPU} ${BUILD_TARGET}"

if [ "$SKIP_TESTS" -eq 0 ]; then
    run_cmd "./perl -I. -Ilib t/mt/build_flags.t"
    if [ -d krt ]; then
        run_cmd "make -C krt test"
    fi
    if [ -f t/mt/simd_parity.t ]; then
        run_cmd "PERL_FORCE_SCALAR=0 ./perl -I. -Ilib t/mt/simd_parity.t"
        run_cmd "PERL_FORCE_SCALAR=1 ./perl -I. -Ilib t/mt/simd_parity.t"
    fi
    if [ -f t/mt/num_digest.t ]; then
        run_cmd "PERL_FORCE_SCALAR=0 ./perl -I. -Ilib t/mt/num_digest.t"
        run_cmd "PERL_FORCE_SCALAR=1 ./perl -I. -Ilib t/mt/num_digest.t"
    fi
fi

run_cmd "make install DESTDIR=\"${STAGE_DIR}\""
if [ "$USE_PGO" -eq 1 ]; then
    run_cmd "make pgo-clean || true"
fi

run_cmd "pkgbuild --root \"${STAGE_DIR}\" \
    --identifier org.perl5mt.runtime \
    --version \"${VERSION}\" \
    --install-location / \
    \"${OUTPUT}\""

echo "Built macOS package: ${OUTPUT}"

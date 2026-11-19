#!/bin/sh
# perl5-mt: install the CPAN modules the XS compartment tests use
# (t/mt/xs_compartment.t) with the fork's own cpan, into $PERL_MT_CPAN
# (default ~/perl5-mt-cpan), from the fork installed at $PERL_MT_PREFIX
# (default ~/perl5-mt-inst; `make install` is run from this tree when the
# installed perl is missing or older than ./perl).
#
# No system package is installed: a module whose C library headers are
# missing on this host is skipped and recorded.  The report goes to
# $PERL_MT_CPAN/fixtures-report.txt and to stdout.
#
#   tools/cpan-fixtures.sh            # from the top of a built tree
#   OPENSSL_PREFIX=/opt/homebrew/opt/openssl tools/cpan-fixtures.sh   # macOS

set -u
cd "$(dirname "$0")/.." || exit 1
TREE=$(pwd)
PREFIX=${PERL_MT_PREFIX:-$HOME/perl5-mt-inst}
CPAN_DIR=${PERL_MT_CPAN:-$HOME/perl5-mt-cpan}
PERL=$PREFIX/bin/perl
REPORT=$CPAN_DIR/fixtures-report.txt

if [ ! -x "$TREE/perl" ]; then
    echo "build the tree first (./perl is missing)" >&2
    exit 1
fi
if [ ! -x "$PERL" ] || [ "$TREE/perl" -nt "$PERL" ]; then
    echo "# installing the fork into $PREFIX"
    make install > "$CPAN_DIR.install.log" 2>&1 || {
        echo "make install failed: see $CPAN_DIR.install.log" >&2
        exit 1
    }
fi

mkdir -p "$CPAN_DIR/home" "$CPAN_DIR/lib/perl5"
ARCH=$("$PERL" -MConfig -e 'print $Config{archname}')
export PERL5LIB="$CPAN_DIR/lib/perl5/$ARCH:$CPAN_DIR/lib/perl5"
export PERL_MM_OPT="INSTALL_BASE=$CPAN_DIR"
export PERL_MB_OPT="--install_base $CPAN_DIR"
export PERL_MM_USE_DEFAULT=1
export NONINTERACTIVE_TESTING=1
export PERL_AUTOINSTALL=--defaultdeps
# a private CPAN configuration and cache for the fork's cpan
export HOME="$CPAN_DIR/home"
unset PERL_LOCAL_LIB_ROOT
: > "$REPORT"

# header probes, without installing anything
have_header() {
    # $1: header path relative to an include dir
    for d in /usr/include /usr/local/include /opt/homebrew/include \
             /opt/local/include ${OPENSSL_PREFIX:+$OPENSSL_PREFIX/include} \
             /usr/include/libxml2 /usr/local/include/libxml2 \
             /opt/homebrew/include/libxml2 \
             "$(xcrun --show-sdk-path 2>/dev/null)/usr/include" \
             "$(xcrun --show-sdk-path 2>/dev/null)/usr/include/libxml2"; do
        [ -n "$d" ] && [ -f "$d/$1" ] && return 0
    done
    return 1
}

report() { echo "$*" | tee -a "$REPORT"; }

install_module() {
    # $1: module, $2: distribution to test with -M, $3: skip reason or ""
    mod=$1; probe=$2; skip=$3
    if [ -n "$skip" ]; then
        report "$mod: SKIPPED ($skip)"
        return
    fi
    pmfile="$(echo "$probe" | sed 's|::|/|g').pm"
    # a module the fork ships (Compress::Zlib and the Compress::Raw::*
    # dists) is used as shipped: cpan would fetch a newer IO-Compress whose
    # Compress::Zlib needs a Compress::Raw::Zlib the core does not have
    core=$(env -u PERL5LIB "$PERL" -M"$probe" -e 'print $INC{$ARGV[0]}' "$pmfile" 2>/dev/null)
    case "$core" in
        "$PREFIX"/*)
            report "$mod: core ($(env -u PERL5LIB "$PERL" -M"$probe" -e 'print $'"$probe"'::VERSION'), $core)"
            if [ -f "$CPAN_DIR/lib/perl5/$pmfile" ]; then
                # an earlier run installed a mismatched copy: take it out
                pk="$CPAN_DIR/lib/perl5/$ARCH/auto/$(dirname "$pmfile")/.packlist"
                for pk in "$pk" "$CPAN_DIR/lib/perl5/$ARCH/auto/IO/Compress/.packlist"; do
                    if [ -f "$pk" ]; then
                        xargs rm -f < "$pk"
                        rm -f "$pk"
                        report "$mod: removed the mismatched copy under $CPAN_DIR listed in $pk"
                    fi
                done
                rm -f "$CPAN_DIR/lib/perl5/$pmfile"
            fi
            return ;;
    esac
    where=$("$PERL" -M"$probe" -e 'print $INC{$ARGV[0]}' "$pmfile" 2> "$CPAN_DIR/$mod.probe.log")
    if [ -n "$where" ]; then
        case "$where" in
            "$CPAN_DIR"/*) report "$mod: already installed ($("$PERL" -M"$probe" -e 'print $'"$probe"'::VERSION'))"; return ;;
        esac
    fi
    echo "# installing $mod"
    CPAN_DIR="$CPAN_DIR" "$PREFIX/bin/cpan" -T -i "$mod" > "$CPAN_DIR/$mod.log" 2>&1
    where=$("$PERL" -M"$probe" -e 'print $INC{$ARGV[0]}' "$pmfile" 2> "$CPAN_DIR/$mod.probe.log")
    if [ -n "$where" ]; then
        report "$mod: installed $("$PERL" -M"$probe" -e 'print $'"$probe"'::VERSION') ($where)"
    else
        report "$mod: FAILED (see $CPAN_DIR/$mod.log and $mod.probe.log)"
    fi
}

export CPAN_DIR
report "# perl5-mt CPAN fixtures on $(uname -sm), $(date), perl $("$PERL" -e 'print $]') at $PREFIX"
report "# fork head: $(git -C "$TREE" rev-parse --short HEAD 2>/dev/null || echo unknown)"

install_module Cpanel::JSON::XS Cpanel::JSON::XS ""
install_module DBI DBI ""
install_module DBD::SQLite DBD::SQLite ""
if have_header libxml/parser.h && { command -v xml2-config >/dev/null 2>&1 || pkg-config --exists libxml-2.0 2>/dev/null; }; then
    install_module XML::LibXML XML::LibXML ""
else
    install_module XML::LibXML XML::LibXML "libxml2 headers or xml2-config/pkg-config not found; no system package installed"
fi
if have_header openssl/ssl.h; then
    install_module Net::SSLeay Net::SSLeay ""
else
    install_module Net::SSLeay Net::SSLeay "openssl headers not found; no system package installed"
fi
if have_header zlib.h; then
    # IO-Compress (Compress::Zlib) wants the matching Compress::Raw::* first
    install_module Compress::Raw::Zlib Compress::Raw::Zlib ""
    install_module Compress::Raw::Bzip2 Compress::Raw::Bzip2 ""
    install_module Compress::Zlib Compress::Zlib ""
else
    install_module Compress::Zlib Compress::Zlib "zlib.h not found; no system package installed"
fi
install_module YAML::XS YAML::XS ""

report "# done: $(grep -c ': installed\|: already' "$REPORT") installed, $(grep -c 'SKIPPED' "$REPORT") skipped, $(grep -c 'FAILED' "$REPORT") failed"

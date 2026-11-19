#
# We will just reuse the AIX hints since we support only building
# for the PASE and the PASE hints are merged with the AIX hints.
#

case "$PASE" in
'')
    if test -d /QOpenSys/usr/lib; then
	PASE='define'
    else
	cat >&4 <<EOF
***
*** This build process only works with the PASE environment (not ILE).
*** You must supply the -DPASE parameter to the Configure script,
*** please read the file README.os400.  Exiting now.
***
EOF
	exit 1
    fi
    ;;
esac

# perl5-mt defaults for IBM i PASE: 64-bit XCOFF + POSIX threads + C11.
case "$OBJECT_MODE" in
    '') OBJECT_MODE=64; export OBJECT_MODE ;;
esac
case "$use64bitall" in
    '') use64bitall="$define" ;;
esac
case "$usethreads" in
    '') usethreads="$define" ;;
esac

cat >&4 <<EOF
***
*** Using the AIX hints file for IBM i PASE, $src/hints/aix.sh.
***
EOF
osname=aix
. $src/hints/aix.sh

#!./perl
# perl5-mt native platform support (AIX, IBM i PASE, z/OS USS ASCII & EBCDIC)
# ABI, dispatch guard, and hints script verification.

BEGIN {
    chdir 't' if -d 't';
    if (-f './test.pl') {
        require './test.pl';
        set_up_inc('../lib') if -d '../lib';
    }
    else {
        require Test::More;
        Test::More->import();
    }
}

use strict;
use warnings;

my $root = (-d '../krt') ? '..' : '.';

sub slurp {
    my ($path) = @_;
    open my $fh, '<', $path or die "open $path: $!";
    local $/;
    my $data = <$fh>;
    close $fh;
    return $data;
}

plan(tests => 17);

# 1. All native platform backend files are present
for my $rel (
    'krt/krt_aix.c',
    'krt/krt_zos.c',
    'mt_ctx_ppc64_aix.c',
    'mt_ctx_s390x.c',
) {
    ok(-f "$root/$rel", "$rel exists");
}

# 2. Platform guards and ABI contracts
my $krt_aix  = slurp("$root/krt/krt_aix.c");
my $krt_zos  = slurp("$root/krt/krt_zos.c");
my $krt_stub = slurp("$root/krt/krt_stubs.c");
my $ctx_aix  = slurp("$root/mt_ctx_ppc64_aix.c");
my $ctx_z    = slurp("$root/mt_ctx_s390x.c");

like($krt_aix,  qr/#if defined\(_AIX\)/,
     'krt_aix.c is gated on _AIX');
like($krt_aix,  qr/__PASE__/,
     'krt_aix.c handles IBM i PASE detection');
like($krt_zos,  qr/#if defined\(__MVS__\)/,
     'krt_zos.c is gated on __MVS__');
like($krt_zos,  qr/PERL_KRT_ZOS_ASCII/,
     'krt_zos.c distinguishes z/OS ASCII and EBCDIC');
like($krt_stub, qr/!defined\(_AIX\)\s*&&\s*!defined\(__MVS__\)/,
     'krt_stubs.c excludes _AIX and __MVS__ now that native backends exist');

like($ctx_aix, qr/\.csect mt_ctx_swap\[DS\]/,
     'mt_ctx_ppc64_aix.c defines XCOFF64 function descriptor for mt_ctx_swap');
like($ctx_aix, qr/mtcrf 0x38, 0/,
     'mt_ctx_ppc64_aix.c restores non-volatile CR2-CR4');
like($ctx_z,   qr/defined\(__MVS__\)\s*&&\s*defined\(__XPLINK__\)/,
     'mt_ctx_s390x.c separates z/OS XPLINK from standard s390x linkage');
like($ctx_z,   qr/stmg %r6, %r14/,
     'mt_ctx_s390x.c saves s390x callee-saved GPRs r6-r14');

# 3. Hints scripts pass POSIX sh syntax check
for my $hint ('hints/aix.sh', 'hints/os400.sh', 'hints/os390.sh') {
    my $rc = system('sh', '-n', "$root/$hint");
    is($rc, 0, "$hint passes sh -n syntax check");
}

# 4. Run compiled krt C ABI test if present
my $abi_bin = "$root/krt/build/none/t_platform_abi";
SKIP: {
    skip 'krt/build/none/t_platform_abi not built yet', 1 unless -x $abi_bin;
    my $out = `$abi_bin 2>&1`;
    my $rc = $?;
    is($rc, 0, 'krt/build/none/t_platform_abi exits 0');
}

#!./perl
# Build optimisation, PGO, symbol-binding, hugepage text, CI, and
# packaging verification.

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

# 1. Configure option plumbing
my $configure = slurp("$root/Configure");
for my $var (qw(
    usepgo usenative usesymbolbinding usehugepagetext
    pgo_compiler pgo_gen_flags pgo_use_flags pgo_lto_flags
    native_cflags symbind_cflags symbind_ldflags hugepage_ldflags
)) {
    like($configure, qr/\b\Q$var\E='\$\Q$var\E'/, "Configure emits $var to config.sh");
}
unlike($configure, qr/-ffast-math|\b-Ofast\b/, 'Configure never introduces -ffast-math or -Ofast');

# 2. cflags.SH preserves -ffp-contract=off and supports native tuning flags
my $cflags_sh = slurp("$root/cflags.SH");
like($cflags_sh, qr/-ffp-contract=off/, 'cflags.SH preserves -ffp-contract=off');
like($cflags_sh, qr/usenative/, 'cflags.SH handles usenative');
like($cflags_sh, qr/usesymbolbinding/, 'cflags.SH handles usesymbolbinding');
like($cflags_sh, qr/PGO_STAGE/, 'cflags.SH handles PGO_STAGE');
unlike($cflags_sh, qr/(?<!imply )-ffast-math|\b-Ofast\b/, 'cflags.SH does not enable -ffast-math or -Ofast');

# 3. Makefile.SH PGO targets and post-link hooks
my $makefile_sh = slurp("$root/Makefile.SH");
for my $target (qw(pgo pgo-gen pgo-train pgo-merge pgo-use pgo-postlink pgo-clean)) {
    like($makefile_sh, qr/^\Q$target\E:/m, "Makefile.SH defines $target target");
}
like($makefile_sh, qr/llvm-profdata/, 'Makefile.SH handles clang llvm-profdata merge');
like($makefile_sh, qr/\.gcda/, 'Makefile.SH handles gcc .gcda profiles');
like($makefile_sh, qr/llvm-bolt/, 'Makefile.SH includes optional Linux BOLT post-link hook');
like($makefile_sh, qr/-order_file/, 'Makefile.SH includes optional macOS ld64 order-file hook');

# 4. Training script, CI workflows, and packaging skeletons
for my $rel (qw(
    tools/pgo-train.pl
    tools/ci-macos.sh
    .forgejo/workflows/nightly-opt.yml
    pkg/rpm/perl5-mt.spec
    pkg/debian/control
    pkg/debian/rules
    pkg/debian/changelog
    pkg/debian/compat
    pkg/macos/build-pkg.sh
)) {
    ok(-f "$root/$rel", "$rel exists");
}

my $nightly = slurp("$root/.forgejo/workflows/nightly-opt.yml");
like($nightly, qr/linux-x86_64/, 'nightly-opt.yml targets linux-x86_64');
like($nightly, qr/linux-ppc64le/, 'nightly-opt.yml targets linux-ppc64le');
like($nightly, qr/-Dusepgo/, 'nightly-opt.yml tests -Dusepgo');
like($nightly, qr/-Dusenative/, 'nightly-opt.yml tests -Dusenative');
like($nightly, qr/make -j"\$\(nproc\)" pgo/, 'nightly-opt.yml runs make pgo');
like($nightly, qr/simd_parity\.t/, 'nightly-opt.yml runs SIMD parity gate');
like($nightly, qr/num_digest\.t/, 'nightly-opt.yml runs numeric digest gate');

my $ci_macos = slurp("$root/tools/ci-macos.sh");
like($ci_macos, qr/--pgo-native/, 'ci-macos.sh supports --pgo-native');
like($ci_macos, qr/-Dusepgo/, 'ci-macos.sh supports -Dusepgo');
like($ci_macos, qr/-Dusenative/, 'ci-macos.sh supports -Dusenative');

# Syntax checks for scripts
{
    my $sh_rc = system('sh', '-n', "$root/tools/ci-macos.sh");
    is($sh_rc, 0, 'sh -n tools/ci-macos.sh passes');

    my $pkg_rc = system('sh', '-n', "$root/pkg/macos/build-pkg.sh");
    is($pkg_rc, 0, 'sh -n pkg/macos/build-pkg.sh passes');

    my $pl_rc = system($^X, "-I$root/lib", '-c', "$root/tools/pgo-train.pl");
    is($pl_rc, 0, 'perl -c tools/pgo-train.pl passes');
}

# 5. Live config.sh and ./cflags verification when configured
if (-f "$root/config.sh" && -x "$root/cflags") {
    my %cfg;
    my $cfg_text = slurp("$root/config.sh");
    while ($cfg_text =~ /^([A-Za-z0-9_]+)='([^']*)'/mg) {
        $cfg{$1} = $2;
    }

    for my $k (qw(usepgo usenative usesymbolbinding usehugepagetext)) {
        ok(exists $cfg{$k} && ($cfg{$k} eq 'define' || $cfg{$k} eq 'undef'),
            "config.sh defines $k ($cfg{$k})");
    }

    my $pp_cmd = `cd "$root" && ./cflags pp.c`;
    chomp $pp_cmd;
    unlike($pp_cmd, qr/-ffast-math|\b-Ofast\b/, './cflags pp.c does not contain -ffast-math or -Ofast');

    if (($cfg{gccversion} // '') ne '') {
        like($pp_cmd, qr/-ffp-contract=off/, './cflags pp.c preserves -ffp-contract=off');
    }

    if (($cfg{usenative} // '') eq 'define') {
        ok(($cfg{native_cflags} // '') ne '', 'native_cflags populated under -Dusenative');
        like($pp_cmd, qr/\Q$cfg{native_cflags}\E/, './cflags pp.c includes native_cflags');
    }

    if (($cfg{usepgo} // '') eq 'define') {
        ok(($cfg{pgo_compiler} // '') =~ /^(?:clang|gcc)$/, "pgo_compiler detected ($cfg{pgo_compiler})");
        my $gen_cmd = `cd "$root" && PGO_STAGE=gen ./cflags pp.c`;
        my $use_cmd = `cd "$root" && PGO_STAGE=use ./cflags pp.c`;
        like($gen_cmd, qr/\Q$cfg{pgo_gen_flags}\E/, 'PGO_STAGE=gen ./cflags pp.c includes pgo_gen_flags');
        like($use_cmd, qr/\Q$cfg{pgo_use_flags}\E/, 'PGO_STAGE=use ./cflags pp.c includes pgo_use_flags');
        like($gen_cmd, qr/-ffp-contract=off/, 'PGO_STAGE=gen preserves -ffp-contract=off');
        like($use_cmd, qr/-ffp-contract=off/, 'PGO_STAGE=use preserves -ffp-contract=off');
    }

    if (($cfg{osname} // '') eq 'darwin') {
        is($cfg{hugepage_ldflags} // '', '', 'hugepage_ldflags is a safe no-op on macOS/Mach-O');
        unlike($cfg{symbind_ldflags} // '', qr/-Bsymbolic-functions/,
            'symbind_ldflags omits ELF-only -Bsymbolic-functions on macOS');
    }
}

done_testing();

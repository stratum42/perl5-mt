#!./perl
# perl5-mt Wave 1: Native Acceleration & Transparent Offload Framework tests.
#
# Runs the standalone accel/ C11 test suite (SIMD ISA probing, registry &
# PERL_ACCEL=off/auto selection, live macOS Accelerate Ozaki CR parity vs
# Shewchuk scalar twin, Mesa OpenCL AMD/Intel-only vendor filter, and
# runtime-probed backend fallback tests).

BEGIN {
    chdir 't' if -d 't';
    if (-f './test.pl') {
        require './test.pl';
        set_up_inc('../lib', '../ext/MT/lib');
    }
}

use strict;
use warnings;

my $root = -d 'accel' ? '.' : (-d '../accel' ? '..' : '');
if (!$root || !-f "$root/accel/Makefile") {
    print "1..0 # SKIP accel/ directory not found\n";
    exit 0;
}

my @tests = qw(
    t_simd_probe
    t_accel_core
    t_accel_cr
    t_accel_opencl_filter
    t_accel_backends
);

my $n = 1 + scalar(@tests);
print "1..$n\n";

my $build_out = `make -C "$root/accel" all 2>&1`;
my $rc = $? >> 8;
if ($rc == 0) {
    print "ok 1 - make -C accel all succeeded\n";
} else {
    print "not ok 1 - make -C accel all failed (exit $rc)\n";
    print "# $_\n" for split /\n/, $build_out;
    exit 1;
}

my $test_out = `make -C "$root/accel" test 2>&1`;
my $test_rc = $? >> 8;

my $idx = 1;
for my $t (@tests) {
    $idx++;
    my $bin = "$root/accel/build/none/$t";
    if (!-x $bin) {
        print "not ok $idx - $t binary missing ($bin)\n";
        next;
    }
    my $out = `"$bin" 2>&1`;
    my $trc = $? >> 8;
    if ($trc == 0 && $out !~ /^not ok /m) {
        print "ok $idx - $t passed\n";
    } else {
        print "not ok $idx - $t failed (exit $trc)\n";
        print "# $_\n" for split /\n/, $out;
    }
}

exit($test_rc ? 1 : 0);

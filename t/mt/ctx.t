#!./perl
# perl5-mt: the fiber context switch (mt_ctx.h and the per-
# architecture mt_ctx_*.c files), exercised through the MT::Ctx self-test.
# A frozen image is process-wide and permanent, so the worker case runs in
# a child perl.

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib');
}

use strict;
use warnings;
use Time::HiRes ();

plan(tests => 6);

sub child {
    my ($code, %opt) = @_;
    my $file = tempfile();
    open my $fh, ">", $file or die "open $file: $!";
    print $fh $code;
    close $fh or die "close $file: $!";
    my $out = runperl(progfile => $file, stderr => 1, %opt);
    $out =~ s/\r\n/\n/g;
    return $out;
}

my $n = $ENV{PERL_MT_LONG_TESTS} ? 10_000_000 : 1_000_000;
my $shstk = MT::Ctx::shstk_active();

ok(defined &MT::Ctx::selftest, 'MT::Ctx::selftest is registered at boot');
ok($shstk == 0 || $shstk == 1, 'shstk_active() is 0 or 1');
is(MT::Ctx::selftest(0), 0, 'a zero-bounce self-test switches nothing');
is(MT::Ctx::selftest(1000), 1000, 'a thousand bounces return the counter');

my $t0 = Time::HiRes::time();
my $got = eval { MT::Ctx::selftest($n) };
my $dt = Time::HiRes::time() - $t0;
if ($shstk) {
    like($@, qr/MT_SHSTK_UNSUPPORTED/,
         "selftest($n) refuses to run with the shadow stack active");
}
else {
    diag($@) if $@;
    is($got, $n, "selftest($n) bounces $n times between two contexts");
    note(sprintf "%.1f ns per swap (%d swaps in %.3f s)",
         $dt * 1e9 / ($n + 1), $n + 1, $dt);
}

SKIP: {
    skip 'the shadow stack is active', 1 if $shstk;
    my $out = child(<<'CODE');
sub f { MT::Ctx::selftest(100_000) }
MT::Image::freeze();
print "worker=", MT::Worker::join(MT::Worker::spawn('main::f')), "\n";
CODE
    like($out, qr/^worker=100000$/m, 'the self-test runs inside a worker');
}

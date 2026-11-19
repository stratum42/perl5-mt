#!./perl
# perl5-mt: process-global immortals, SVf_IMMORTAL and the
# refcount / mutation guards.

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib');
}

use strict;
use warnings;
use Config;

plan(tests => 32);

require Scalar::Util;

my $yes   = \!!1;
my $no    = \!!0;
my $undef = \undef;

# Devel::Peek shows the flags; B reports the immortals as B::SPECIAL and
# hides them, so a dump from a child perl is the portable way to see
# SVf_IMMORTAL.  $setup runs first, then $expr is dumped.
sub peek {
    my ($setup, $expr) = @_;
    $expr = $setup, $setup = '' unless defined $expr;
    return runperl(prog => "use Devel::Peek; $setup Dump($expr)", stderr => 1);
}
sub flags_of {
    my $d = peek($_[0]);
    $d =~ /^\s*FLAGS = \((.*?)\)/m ? $1 : "<no FLAGS in: $d>";
}
sub type_of {
    my $d = peek($_[0]);
    $d =~ /^SV = (\w+)/m ? $1 : "<no SV in: $d>";
}
sub addr { Scalar::Util::refaddr($_[0]) }

# ---- 2.2: the flag and the type field ---------------------------------------

like(flags_of('!!1'),   qr/\bIMMORTAL\b/, 'PL_sv_yes carries SVf_IMMORTAL');
like(flags_of('!!0'),   qr/\bIMMORTAL\b/, 'PL_sv_no carries SVf_IMMORTAL');
like(flags_of('undef'), qr/\bIMMORTAL\b/, 'PL_sv_undef carries SVf_IMMORTAL');
unlike(flags_of('my $p = 1;', '$p'), qr/\bIMMORTAL\b/, 'an ordinary scalar does not');
unlike(flags_of('[]'), qr/\bIMMORTAL\b/, 'nor an array');
unlike(flags_of('{}'), qr/\bIMMORTAL\b/, 'nor a hash');
unlike(flags_of('my $r = [];', '$r'), qr/\bIMMORTAL\b/, 'nor a reference');
is(type_of('undef'), 'NULL', 'PL_sv_undef is still SVt_NULL (type field intact)');
is(type_of('!!1'),   'PVNV', 'PL_sv_yes is still SVt_PVNV');
is(type_of('[1]'), 'IV', 'a reference dumps as IV');
like(peek('[1]'), qr/^  SV = PVAV/m,
     'arrays keep their type with the 5-bit field');

# ---- 2.1: process-global, same address in every interpreter ---------------

is(addr($yes), addr(\!!1), 'every !!1 is the same SV');
is(addr($no),  addr(\!!0), 'every !!0 is the same SV');
isnt(addr($yes), addr($no), 'yes and no differ');

SKIP: {
    skip 'no ithreads', 4 unless $Config{useithreads};
    require threads;
    my @got = map {
        threads->create(sub { [ addr(\!!1), addr(\!!0), addr(\undef) ] })->join
    } 1 .. 4;
    is($got[$_][0], addr($yes), "thread $_ sees PL_sv_yes at the same address")
        for 0, 1;
    is($got[2][1], addr($no),    'thread 2 sees PL_sv_no at the same address');
    is($got[3][2], addr($undef), 'thread 3 sees PL_sv_undef at the same address');
}

# ---- 2.3: the refcount is never written ------------------------------------

{
    my $rc0 = Internals::SvREFCNT($$yes);
    my @many = map { \!!1 } 1 .. 1000;
    is(Internals::SvREFCNT($$yes), $rc0,
       'taking 1000 references leaves the immortal refcount alone');
    @many = ();
    is(Internals::SvREFCNT($$yes), $rc0, '... and dropping them too');
    ok($rc0 > 1000, 'the immortal refcount is the saturated value');
}
{
    my $rc0 = Internals::SvREFCNT($$undef);
    my @a = (undef) x 2000;
    my %h = map { $_ => undef } 1 .. 500;
    @a = (); %h = ();
    is(Internals::SvREFCNT($$undef), $rc0,
       'PL_sv_undef refcount survives aggregate churn');
}
eval { Internals::SvREFCNT($$yes, 5) };
like($@, qr/MT_IMMORTAL_MUTATION/, 'Internals::SvREFCNT setter croaks');

# ---- 2.3: mutation guards ---------------------------------------------------

eval { Internals::SvREADONLY($$yes, 0) };
is($@, '', 'Internals::SvREADONLY(immortal, 0) is a silent no-op');
ok(Internals::SvREADONLY($$yes), '... and it stays read-only');
eval { $$undef = 1 };
like($@, qr/read-only/, 'assigning to the immortal is still a read-only error');

# ---- 2.3: weaken is a no-op on immortal targets ----------------------------

{
    my $r = \!!1;
    Scalar::Util::weaken($r);
    ok(!Scalar::Util::isweak($r), 'weaken(\\PL_sv_yes) leaves the ref strong');
    ok(defined $r && $$r, '... and the target is still reachable');
    my $u = \undef;
    Scalar::Util::weaken($u);
    ok(!Scalar::Util::isweak($u), 'weaken(\\PL_sv_undef) is a no-op too');
    is(addr($u), addr($undef), '... pointing at the same immortal');
}

# ---- sharing ----------------------------------------------------------------

SKIP: {
    skip 'no threads::shared', 2 unless $Config{useithreads};
    require threads::shared;
    eval { threads::shared::share($yes) };
    like($@, qr/MT_IMMORTAL_MUTATION/, 'share() of an immortal croaks');
    my $v = 1;
    eval { threads::shared::share(\$v) };
    is($@, '', 'share() of an ordinary scalar still works');
}

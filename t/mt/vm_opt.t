#!./perl
# perl5-mt: core interpreter / VM optimizations
# Covers 8a (RC stack hygiene), 8b (entersub/leavesub + signatures),
# 8c (superinstructions + B::Deparse), 8d (overlay-validated inline caches),
# and 8e (freeze-time specialisation + PERL_MT_NO_SPECIALIZE=1).

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib');
}

use strict;
use warnings;
use feature 'signatures';
no warnings 'experimental::signatures';
use B::Deparse;

plan(tests => 50);

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

# ---- 8b: entersub / leavesub & signature fast paths -----------------------

sub zero_arg { return 42 }
sub one_arg  { my ($a) = @_; return $a * 2 }
sub four_arg { my ($a, $b, $c, $d) = @_; return $a + $b + $c + $d }
sub many_arg { my $s = 0; $s += $_ for @_; return $s }
sub sig_req ($x, $y) { return $x * 10 + $y }
sub sig_opt ($x, $y = 7) { return $x + $y }
sub sig_slurpy_arr ($head, @rest) { return "$head:" . join(",", @rest) }
sub sig_slurpy_hash ($head, %kv) {
    return "$head:" . join(",", map { "$_=$kv{$_}" } sort keys %kv);
}
sub fact_sig ($n) { return $n <= 1 ? 1 : $n * fact_sig($n - 1) }

is(zero_arg(), 42, '8b: 0-arg sub scalar immortal return');
is(one_arg(21), 42, '8b: 1-arg sub');
is(four_arg(1, 2, 3, 4), 10, '8b: 4-arg unrolled copy');
is(many_arg(1..10), 55, '8b: >4-arg sub copy');
is(sig_req(4, 2), 42, '8b: signature required params fast path');
is(sig_opt(10), 17, '8b: signature optional param default');
is(sig_opt(10, 5), 15, '8b: signature optional param supplied');
is(sig_slurpy_arr('a', 1, 2, 3), 'a:1,2,3', '8b: signature slurpy array');
is(sig_slurpy_hash('h', b => 2, a => 1), 'h:a=1,b=2', '8b: signature slurpy hash');
is(fact_sig(6), 720, '8b: recursive signature sub');

# Signature error paths must still croak with exact Perl diagnostics
eval { sig_req(1) };
like($@, qr/Too few arguments for subroutine/, '8b: signature too few args croaks');
eval { sig_req(1, 2, 3) };
like($@, qr/Too many arguments for subroutine/, '8b: signature too many args croaks');
eval { sig_slurpy_hash('h', 'odd') };
like($@, qr/Odd name\/value argument for subroutine/, '8b: signature odd hash croaks');

# 8a: PADTMP arguments passed to Perl subs and XSUBs
{
    my ($u, $v) = (3, 4);
    is(one_arg($u + $v), 14, '8a: PADTMP argument to Perl sub');
    is(sig_req($u + 1, $v * 2), 48, '8a: PADTMP arguments to signature sub');
    is(MT::Image::frozen() ? 1 : 0, 0, '8a: MT::Image::frozen XSUB works');
}

# ---- 8c: superinstructions (compare-and-branch, shift+store, method+sub) --

{
    my ($a, $b) = (5, 10);
    my $r = '';
    $r .= 'eq' if $a == 5;
    $r .= 'ne' if $a != $b;
    $r .= 'lt' if $a < $b;
    $r .= 'gt' if $b > $a;
    $r .= 'le' if $a <= 5;
    $r .= 'ge' if $b >= 10;
    is($r, 'eqnelgt' =~ s/lgt/ltgtlege/r, '8c: compare-and-branch true branches');

    my $f = '';
    $f .= '1' if $a == $b;
    $f .= '2' if $a != 5;
    $f .= '3' if $b < $a;
    $f .= '4' if $a > $b;
    $f .= '5' if $b <= $a;
    $f .= '6' if $a >= $b;
    is($f, '', '8c: compare-and-branch false branches');

    # Loop using fused lt + cond_expr
    my $sum = 0;
    my $i = 0;
    while ($i < 100) {
        $sum += $i;
        $i++;
    }
    is($sum, 4950, '8c: while ($i < 100) fused compare-and-branch');

    # NaN semantics must be preserved in fused compare-and-branch
    my $nan = 0 + "NaN";
    my $nan_res = '';
    $nan_res .= 'eq' if $nan == $nan;
    $nan_res .= 'ne' if $nan != $nan;
    $nan_res .= 'lt' if $nan < 1;
    $nan_res .= 'gt' if $nan > 1;
    $nan_res .= 'le' if $nan <= 1;
    $nan_res .= 'ge' if $nan >= 1;
    is($nan_res, 'ne', '8c: NaN compare-and-branch semantics preserved');

    # defined + cond_expr
    my ($def_v, $undef_v) = (0, undef);
    is((defined($def_v) ? 'yes' : 'no'), 'yes', '8c: defined(0) fused branch');
    is((defined($undef_v) ? 'yes' : 'no'), 'no', '8c: defined(undef) fused branch');
}

# Overloaded numeric comparisons must fall back cleanly through cond_expr
{
    package OvlNum;
    use overload
        '==' => sub { $_[0]->{v} == (ref $_[1] ? $_[1]->{v} : $_[1]) },
        '<'  => sub { $_[0]->{v} <  (ref $_[1] ? $_[1]->{v} : $_[1]) },
        fallback => 1;
    sub new { bless { v => $_[1] }, $_[0] }
}
{
    my $x = OvlNum->new(7);
    my $y = OvlNum->new(9);
    is(($x == 7 ? 'ok' : 'bad'), 'ok', '8c: overloaded == in compare-and-branch');
    is(($x < $y ? 'ok' : 'bad'), 'ok', '8c: overloaded < in compare-and-branch');
    is(($y < $x ? 'bad' : 'ok'), 'ok', '8c: overloaded < false in compare-and-branch');
}

# Fused shift + padsv_store
{
    sub shift_self_and_arg {
        my $self = shift;
        my $val  = shift;
        return "$self:$val";
    }
    my @q = qw(alpha beta gamma);
    my $first = shift @q;
    my $second = shift @q;
    is($first, 'alpha', '8c: fused shift @a -> padsv_store (1)');
    is($second, 'beta', '8c: fused shift @a -> padsv_store (2)');
    is(scalar(@q), 1, '8c: array shortened properly');
    is(shift_self_and_arg('obj', 99), 'obj:99', '8c: fused shift @_ -> padsv_store');

    my @empty;
    my $none = shift @empty;
    ok(!defined($none), '8c: fused shift on empty array yields undef');
}

# Fused method_named + entersub
{
    package MethBox;
    sub new { my $cls = shift; my $v = shift; bless { v => $v }, $cls }
    sub get { my $self = shift; return $self->{v} }
    sub add { my $self = shift; my $d = shift; return $self->{v} + $d }
}
{
    my $b = MethBox->new(30);
    is($b->get(), 30, '8c: fused method_named + entersub (0 args)');
    is($b->add(12), 42, '8c: fused method_named + entersub (1 arg)');
}

# B::Deparse preservation across all 8c superinstructions
{
    my $dp = B::Deparse->new;
    my $sub_cmp = sub {
        my ($x, $y) = @_;
        return 1 if $x == $y;
        return 2 if $x < $y;
        return 3 if defined $x;
        return 0;
    };
    my $src_cmp = $dp->coderef2text($sub_cmp);
    like($src_cmp, qr/\$x == \$y/, '8c: B::Deparse preserves == in compare-and-branch');
    like($src_cmp, qr/\$x < \$y/,  '8c: B::Deparse preserves < in compare-and-branch');
    like($src_cmp, qr/defined \$x/, '8c: B::Deparse preserves defined in compare-and-branch');

    my $sub_shift = sub {
        my $self = shift;
        my @a = @_;
        my $first = shift @a;
        return $self->get($first);
    };
    my $src_shift = $dp->coderef2text($sub_shift);
    like($src_shift, qr/shift(?:\(\)| \@_)/, '8c: B::Deparse preserves shift @_');
    like($src_shift, qr/shift \@a/, '8c: B::Deparse preserves shift @a');
    like($src_shift, qr/\$self->get\(\$first\)/, '8c: B::Deparse preserves method_named + entersub');
}

# ---- 8d & 8e: overlay-validated inline caches & freeze-time specialisation -

my $worker_script = <<'CODE';
use strict;
use warnings;

package BaseCls;
sub tag { return 'base' }
sub calc { my ($self, $x) = @_; return $x + 1 }

package SubCls;
our @ISA = ('BaseCls');
sub tag { return 'sub' }

package AltBase;
sub calc { my ($self, $x) = @_; return $x + 100 }

package main;
our %FROZEN_CFG = (alpha => 'A1', beta => 'B2', gamma => 'G3');
our $SHARED_OUR = 77;
sub CONST_ANS { 42 }

sub run_worker {
    my ($mode) = @_;
    my $obj = bless {}, 'SubCls';

    # Before any overlay writes: hits 8e constant-sub, 8e devirt, 8d meth_ic, 8d helem_ic
    my $c1 = CONST_ANS();
    my $m1 = SubCls->tag();
    my $m2 = $obj->calc(5);
    my $h1 = $FROZEN_CFG{alpha};
    my $g1 = $SHARED_OUR;

    if ($mode eq 'normal') {
        return join('|', $c1, $m1, $m2, $h1, $g1);
    }
    elsif ($mode eq 'override_sub') {
        no warnings 'redefine';
        *CONST_ANS = sub { 999 };
        *SubCls::tag = sub { 'overridden_sub' };
        my $c2 = CONST_ANS();
        my $m3 = SubCls->tag();
        return join('|', $c1, $c2, $m1, $m3);
    }
    elsif ($mode eq 'override_isa') {
        @SubCls::ISA = ('AltBase');
        my $m3 = $obj->calc(5);
        return join('|', $m2, $m3);
    }
    elsif ($mode eq 'override_hash') {
        $FROZEN_CFG{alpha} = 'A1_NEW';
        my $h2 = $FROZEN_CFG{alpha};
        my $h3 = $FROZEN_CFG{beta};
        $SHARED_OUR = 88;
        return join('|', $h1, $h2, $h3, $g1, $SHARED_OUR);
    }
    return 'unknown';
}

MT::Image::freeze();
my $st = MT::Image::stats();
print "STATS specialize=$st->{specialize} meth_ic=$st->{n_meth_ic} helem_ic=$st->{n_helem_ic} spec_const=$st->{n_spec_const_subs} spec_devirt=$st->{n_spec_meth_devirt}\n";

for my $mode (qw(normal override_sub override_isa override_hash)) {
    my $id = MT::Worker::spawn('main::run_worker', $mode);
    my $res = MT::Worker::join($id);
    print "$mode=$res\n";
}
# Verify worker isolation: a fresh normal worker still sees pristine image values
{
    my $id = MT::Worker::spawn('main::run_worker', 'normal');
    my $res = MT::Worker::join($id);
    print "fresh=$res\n";
}
CODE

SKIP: {
    skip 'MT::Worker not available', 12 unless defined &MT::Worker::spawn;

    # Default mode (specialisation enabled)
    my $out_on = do {
        local $ENV{PERL_MT_NO_SPECIALIZE};
        delete $ENV{PERL_MT_NO_SPECIALIZE};
        child($worker_script);
    };
    like($out_on, qr/^STATS specialize=1 /m, '8e: freeze-time specialisation enabled by default');
    like($out_on, qr/\bmeth_ic=[1-9]\d*\b/, '8d: method inline cache populated at freeze');
    like($out_on, qr/\bhelem_ic=[1-9]\d*\b/, '8d: constant-key hash inline cache populated at freeze');
    like($out_on, qr/\bspec_const=[1-9]\d*\b/, '8e: constant frozen sub call site specialised');
    like($out_on, qr/\bspec_devirt=[1-9]\d*\b/, '8e: static class method call site devirtualised');
    like($out_on, qr/^normal=42\|sub\|6\|A1\|77$/m, '8d/8e: worker hits inline caches & specialisations');
    like($out_on, qr/^override_sub=42\|999\|sub\|overridden_sub$/m, '8d/8e: worker sub/method redefine invalidates cache');
    like($out_on, qr/^override_isa=6\|105$/m, '8d: worker @ISA change invalidates method cache');
    like($out_on, qr/^override_hash=A1\|A1_NEW\|B2\|77\|88$/m, '8d: worker hash/glob write invalidates cache');
    like($out_on, qr/^fresh=42\|sub\|6\|A1\|77$/m, '8d/8e: fresh worker stays isolated from other workers overlays');

    # PERL_MT_NO_SPECIALIZE=1 kill-switch mode
    local $ENV{PERL_MT_NO_SPECIALIZE} = 1;
    my $out_off = child($worker_script);
    like($out_off, qr/^STATS specialize=0 meth_ic=0 helem_ic=0 spec_const=0 spec_devirt=0$/m,
         '8e: PERL_MT_NO_SPECIALIZE=1 disables specialisation cleanly');

    # Parity check: all worker outputs must be byte-for-byte identical
    (my $norm_on  = $out_on)  =~ s/^STATS .*\n//m;
    (my $norm_off = $out_off) =~ s/^STATS .*\n//m;
    is($norm_on, $norm_off, '8e: exact parity between specialised and PERL_MT_NO_SPECIALIZE=1');
}

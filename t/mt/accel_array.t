#!./perl
use strict;
use warnings;
use Test::More;
use MT::Array;
use MT::Accel;

sub dhex { unpack("H*", pack("d", $_[0])) }

sub build_ill_conditioned_vectors {
    my ($n) = @_;
    my $seed = 0x51a7c0de;
    my $rng = sub {
        $seed = ($seed * 1103515245 + 12345) & 0x7fffffff;
        return $seed;
    };

    my (@a, @b);
    for my $i (0 .. $n - 1) {
        my $r = $i % 8;
        if ($r == 0) {
            push @a, 1e16;
            push @b, 1.0;
        } elsif ($r == 1) {
            push @a, -1e16;
            push @b, 1.0;
        } elsif ($r == 2) {
            push @a, 1.0;
            push @b, 1e15;
        } elsif ($r == 3) {
            push @a, 1.0;
            push @b, -1e15;
        } elsif ($r == 4) {
            push @a, 1e-16 * (($i % 17) - 8);
            push @b, 0.75;
        } elsif ($r == 5) {
            push @a, 2.0 ** -53;
            push @b, 1.0;
        } else {
            my $e = ($rng->() % 40) - 20;
            my $m = (($rng->() % 2000001) - 1000000) / 1000000.0;
            push @a, $m * (2.0 ** $e);
            my $e2 = ($rng->() % 30) - 15;
            my $m2 = (($rng->() % 2000001) - 1000000) / 1000000.0;
            push @b, $m2 * (2.0 ** $e2);
        }
    }
    return (\@a, \@b);
}

# Child parity harness: exercises above-threshold Accelerate Ozaki CR vs scalar
if (@ARGV && $ARGV[0] eq '--parity') {
    my ($a_raw, $b_raw) = build_ill_conditioned_vectors(8192);
    my $a = MT::Array::packed('NV', @$a_raw);
    my $b = MT::Array::packed('NV', @$b_raw);

    MT::Accel::reset_stats();
    my $s_a = MT::Array::sum($a);
    my $s_b = MT::Array::sum($b);
    my $dot = MT::Array::dot($a, $b);

    my $vadd = MT::Array::vec_add($a, $b);
    my $vmul = MT::Array::vec_mul($a, $b);
    my $vax  = MT::Array::axpy(-2.375, $a, $b);

    my $s_add = MT::Array::sum($vadd);
    my $s_mul = MT::Array::sum($vmul);
    my $s_ax  = MT::Array::sum($vax);

    my $u8_a = MT::Array::packed('U8', map { ($_ * 37) & 0xff } 0 .. 2047);
    my $u8_b = MT::Array::packed('U8', map { ($_ * 53 + 7) & 0xff } 0 .. 2047);
    my $di8  = MT::Array::dot_i8($u8_a, $u8_b);

    my $st = MT::Accel::stats();
    print join(" ",
        "PARITY",
        dhex($s_a), dhex($s_b), dhex($dot),
        dhex($s_add), dhex($s_mul), dhex($s_ax),
        $di8,
        "STATS",
        MT::Accel::active(),
        $st->{offload_calls},
        $st->{fallback_calls},
        ($st->{last_backend} // 'none'),
    ) . "\n";
    exit 0;
}

my $is_darwin = ($^O eq 'darwin');
my $env_off = (defined $ENV{PERL_ACCEL} && $ENV{PERL_ACCEL} =~ /^(?:off|0|none)$/)
           || (defined $ENV{PERL_FORCE_SCALAR} && $ENV{PERL_FORCE_SCALAR} ne '' && $ENV{PERL_FORCE_SCALAR} ne '0');

# 1. MT::Accel introspection: backends(), active(), simd_level(), simd_caps()
{
    my $backends = MT::Accel::backends();
    is(ref($backends), 'ARRAY', 'MT::Accel::backends() returns arrayref');
    cmp_ok(scalar(@$backends), '>=', 9, 'all built-in backends registered');

    my %by_name;
    my $has_nvidia = 0;
    for my $b (@$backends) {
        $by_name{$b->{name}} = $b;
        if (($b->{name} // '') =~ /nvidia|cuda/i
            || ($b->{description} // '') =~ /nvidia|cuda/i)
        {
            $has_nvidia = 1;
        }
    }
    ok(!$has_nvidia, 'MT::Accel::backends() never exposes NVIDIA or CUDA');
    ok(exists $by_name{scalar},     'scalar backend present');
    ok(exists $by_name{accelerate}, 'accelerate backend present');

    if ($is_darwin && !$env_off) {
        is(MT::Accel::active(), 'accelerate', 'macOS auto mode activates accelerate backend');
        ok(MT::Accel::enabled(), 'MT::Accel::enabled() is true in auto mode');
        is($by_name{accelerate}{available}, 1, 'accelerate backend is available on macOS');
        is($by_name{accelerate}{active},    1, 'accelerate backend is marked active on macOS');
        like($by_name{accelerate}{description}, qr/macOS Accelerate/i,
             'accelerate backend description identifies macOS Accelerate');
    } elsif ($env_off) {
        is(MT::Accel::active(), 'scalar', 'PERL_ACCEL=off activates scalar backend');
        ok(!MT::Accel::enabled(), 'MT::Accel::enabled() is false when disabled');
        is($by_name{scalar}{active}, 1, 'scalar backend is marked active when disabled');
    } else {
        ok(length(MT::Accel::active()) > 0, 'MT::Accel::active() returns non-empty name');
    }

    my $level = MT::Accel::simd_level();
    ok(defined $level && length($level) > 0, "MT::Accel::simd_level() returns '$level'");
    my $caps = MT::Accel::simd_caps();
    is(ref($caps), 'HASH', 'MT::Accel::simd_caps() returns hashref');
    ok(exists $caps->{bits}, 'simd_caps includes raw bits mask');
    if ($is_darwin && !$env_off) {
        ok($caps->{neon} || $caps->{sse2}, 'macOS host reports NEON or SSE2 capability');
    }
}

# 2. Vector operations: vec_add, vec_mul, axpy, dot_i8
{
    my $a = MT::Array::packed('NV', 1.5, -2.25, 3.0, 4.5);
    my $b = MT::Array::packed('NV', 0.5,  2.25, -1.0, 2.0);

    my $add = MT::Array::vec_add($a, $b);
    is(MT::Array::is_packed($add), 'NV', 'vec_add returns packed NV array');
    is_deeply([@$add], [2.0, 0.0, 2.0, 6.5], 'vec_add values exact');

    my $mul = MT::Array::vec_mul($a, $b);
    is(MT::Array::is_packed($mul), 'NV', 'vec_mul returns packed NV array');
    is_deeply([@$mul], [0.75, -5.0625, -3.0, 9.0], 'vec_mul values exact');

    my $ax = MT::Array::axpy(2.0, $a, $b);
    is(MT::Array::is_packed($ax), 'NV', 'axpy returns packed NV array');
    is_deeply([@$ax], [3.5, -2.25, 5.0, 11.0], 'axpy (alpha*x + y) values exact');

    # Plain array inputs also work
    my $p_add = MT::Array::vec_add([1, 2, 3], [4, 5, 6]);
    is_deeply([@$p_add], [5, 7, 9], 'vec_add works on plain arrays');
    my $p_mul = MT::Array::vec_mul([2, 3, 4], [5, 6, 7]);
    is_deeply([@$p_mul], [10, 18, 28], 'vec_mul works on plain arrays');
    my $p_ax  = MT::Array::axpy(-1.5, [2, 4], [10, 20]);
    is_deeply([@$p_ax], [7, 14], 'axpy works on plain arrays');

    # Empty arrays
    my $e1 = MT::Array::packed('NV');
    my $e2 = MT::Array::packed('NV');
    is(scalar(@{ MT::Array::vec_add($e1, $e2) }), 0, 'vec_add empty');
    is(scalar(@{ MT::Array::vec_mul($e1, $e2) }), 0, 'vec_mul empty');
    is(scalar(@{ MT::Array::axpy(3.0, $e1, $e2) }), 0, 'axpy empty');
    is(MT::Array::dot_i8([], []), 0, 'dot_i8 empty');

    # Length mismatch croaks
    eval { MT::Array::vec_add([1, 2], [1]) };
    like($@, qr/length mismatch/, 'vec_add croaks on length mismatch');
    eval { MT::Array::vec_mul([1, 2], [1]) };
    like($@, qr/length mismatch/, 'vec_mul croaks on length mismatch');
    eval { MT::Array::axpy(1.0, [1, 2], [1]) };
    like($@, qr/length mismatch/, 'axpy croaks on length mismatch');
    eval { MT::Array::dot_i8([1, 2], [1]) };
    like($@, qr/length mismatch/, 'dot_i8 croaks on length mismatch');

    # dot_i8 on signed IV / plain arrays and two's-complement packed U8 arrays
    my $i8_a = MT::Array::packed('IV', -128, -1, 0, 1, 127);
    my $i8_b = MT::Array::packed('IV',    2, -3, 5, 7,  -2);
    is(MT::Array::dot_i8($i8_a, $i8_b), (-128*2 + (-1)*(-3) + 0 + 7 + 127*(-2)),
       'dot_i8 exact on packed IV signed int8 values');

    is(MT::Array::dot_i8([-10, 20, -30], [3, -4, 5]), (-30 - 80 - 150),
       'dot_i8 exact on plain signed arrays');

    # Packed U8 zero-copy (255 == -1 in int8_t)
    my $u8_x = MT::Array::packed('U8', 10, 255, 128); # 10, -1, -128
    my $u8_y = MT::Array::packed('U8',  3,   4,   2); #  3,  4,    2
    is(MT::Array::dot_i8($u8_x, $u8_y), (10*3 + (-1)*4 + (-128)*2),
       'dot_i8 exact on packed U8 two\'s-complement bytes');

    eval { MT::Array::dot_i8([200], [1]) };
    like($@, qr/out of int8 range/, 'dot_i8 rejects out-of-range plain element');
}

# 3. Live offload dispatch & stats tracking on large arrays
{
    my ($a_raw, $b_raw) = build_ill_conditioned_vectors(8192);
    my $a = MT::Array::packed('NV', @$a_raw);
    my $b = MT::Array::packed('NV', @$b_raw);
    my $u8_a = MT::Array::packed('U8', map { $_ & 0x7f } 0 .. 2047);
    my $u8_b = MT::Array::packed('U8', map { (2047 - $_) & 0x7f } 0 .. 2047);

    MT::Accel::reset_stats();
    my $s   = MT::Array::sum($a);
    my $d   = MT::Array::dot($a, $b);
    my $va  = MT::Array::vec_add($a, $b);
    my $vm  = MT::Array::vec_mul($a, $b);
    my $vx  = MT::Array::axpy(1.5, $a, $b);
    my $di8 = MT::Array::dot_i8($u8_a, $u8_b);

    ok(defined $s && defined $d && defined $di8, 'large ops produced defined results');
    is(scalar(@$va), 8192, 'large vec_add length 8192');
    is(scalar(@$vm), 8192, 'large vec_mul length 8192');
    is(scalar(@$vx), 8192, 'large axpy length 8192');

    my $st = MT::Accel::stats();
    if ($is_darwin && !$env_off) {
        is($st->{offload_calls}, 6, 'all 6 above-threshold ops offloaded to macOS Accelerate');
        is($st->{fallback_calls}, 0, 'zero scalar fallbacks for above-threshold ops on macOS');
        is($st->{last_backend}, 'accelerate', 'last_backend is accelerate');
    } elsif ($env_off) {
        is($st->{offload_calls}, 0, 'zero offload calls under PERL_ACCEL=off');
        is($st->{fallback_calls}, 6, 'all 6 ops ran scalar twin under PERL_ACCEL=off');
        is($st->{last_backend}, 'scalar', 'last_backend is scalar under PERL_ACCEL=off');
    }
}

# 4. Bit-identical parity: PERL_ACCEL=auto vs PERL_ACCEL=off vs PERL_FORCE_SCALAR=1
{
    my $inc = -f 'lib/strict.pm' ? '-I. -Ilib' : '-I.. -I../lib';
    my $out_auto = do {
        local $ENV{PERL_ACCEL} = 'auto';
        local $ENV{PERL_FORCE_SCALAR} = 0;
        `$^X $inc $0 --parity`;
    };
    is($?, 0, 'PERL_ACCEL=auto parity child exited 0');

    my $out_off = do {
        local $ENV{PERL_ACCEL} = 'off';
        local $ENV{PERL_FORCE_SCALAR} = 0;
        `$^X $inc $0 --parity`;
    };
    is($?, 0, 'PERL_ACCEL=off parity child exited 0');

    my $out_scalar = do {
        local $ENV{PERL_ACCEL} = 'auto';
        local $ENV{PERL_FORCE_SCALAR} = 1;
        `$^X $inc $0 --parity`;
    };
    is($?, 0, 'PERL_FORCE_SCALAR=1 parity child exited 0');

    chomp($out_auto, $out_off, $out_scalar);

    my ($vals_auto, $stats_auto)     = split / STATS /, $out_auto, 2;
    my ($vals_off, $stats_off)       = split / STATS /, $out_off, 2;
    my ($vals_scalar, $stats_scalar) = split / STATS /, $out_scalar, 2;

    like($vals_auto, qr/^PARITY /, 'parity auto output format valid');
    is($vals_auto, $vals_off,
       'macOS Accelerate Ozaki CR sum/dot/vec_*/dot_i8 bit-identical to PERL_ACCEL=off');
    is($vals_auto, $vals_scalar,
       'macOS Accelerate Ozaki CR sum/dot/vec_*/dot_i8 bit-identical to PERL_FORCE_SCALAR=1');

    if ($is_darwin) {
        like($stats_auto, qr/^accelerate [1-9]\d* 0 accelerate$/,
             "auto child verified live Accelerate offload ($stats_auto)");
    }
    like($stats_off,    qr/^scalar 0 [1-9]\d* scalar$/,
         "PERL_ACCEL=off child verified scalar twin ($stats_off)");
    like($stats_scalar, qr/^scalar 0 [1-9]\d* scalar$/,
         "PERL_FORCE_SCALAR=1 child verified scalar twin ($stats_scalar)");
}

done_testing();

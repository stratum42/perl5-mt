#!./perl
use strict;
use warnings;
use Test::More;
use Config;

BEGIN {
    chdir 't' if -d 't' && ! -d 'mt';
    unshift @INC, '../lib', 'lib', '.';
}

use MT::Decimal;
use Digest::SHA qw(sha256_hex);

sub dhex {
    my ($v) = @_;
    return unpack('H16', pack('d>', $v));
}

sub digest_rows {
    my (@rows) = @_;
    return sha256_hex(join("\n", @rows) . "\n");
}

my @all_rows;

# =========================================================================
# 1. Contraction flags & basic arithmetic corpus
# =========================================================================
{
    my $cflags_path = -f '../cflags' ? '../cflags' : 'cflags';
    if (-f $cflags_path) {
        open my $fh, '<', $cflags_path or die "open $cflags_path: $!";
        local $/;
        my $txt = <$fh>;
        close $fh;
        like($txt, qr/-ffp-contract=off/, 'cflags contains -ffp-contract=off');
    } else {
        pass('cflags not present in standalone run');
    }

    # FMA-sensitive test: (1 + 2^-27) * (1 + 2^-27) - (1 + 2^-26)
    # Without FMA contraction, product rounds to 1 + 2^-26 so difference is 0.0.
    my $a = 1.0 + (2.0 ** -27);
    my $b = 1.0 + (2.0 ** -27);
    my $c = -(1.0 + (2.0 ** -26));
    my $fma_check = ($a * $b) + $c;
    is(dhex($fma_check), '0000000000000000', 'no FMA contraction on a*b + c');

    my @arith_rows;
    my @vals = (0.0, -0.0, 1.0, -1.0, 0.1, 0.2, 0.3, 1.5, 3.141592653589793, 1e100, 1e-100);
    for my $i (0 .. $#vals) {
        my $x = $vals[$i];
        my $y = $vals[($i + 3) % @vals];
        push @arith_rows, "add:$i:" . dhex($x + $y);
        push @arith_rows, "sub:$i:" . dhex($x - $y);
        push @arith_rows, "mul:$i:" . dhex($x * $y);
        push @arith_rows, "div:$i:" . dhex($x / $y) if $y != 0.0;
        push @arith_rows, "int:$i:" . dhex(int($x));
        push @arith_rows, "neg:$i:" . dhex(-$x);
        push @arith_rows, "abs:$i:" . dhex(abs($x));
    }
    # Large double modulo (exercises cr_fmod in pp_modulo)
    my $big_mod = (1e22) % (3.0);
    push @arith_rows, "mod_big:" . dhex($big_mod);
    ok($big_mod >= 0 && $big_mod < 3.0, 'large double modulo in range');

    my $d = digest_rows(@arith_rows);
    note("arith_digest=$d");
    push @all_rows, @arith_rows;
}

# =========================================================================
# 2. Correctly rounded parsing (Eisel-Lemire + exact fallback)
# =========================================================================
{
    my @parse_cases = (
        [ '0',                        '0000000000000000' ],
        [ '-0.0',                     '8000000000000000' ],
        [ '0.1',                      '3fb999999999999a' ],
        [ '0.2',                      '3fc999999999999a' ],
        [ '0.3',                      '3fd3333333333333' ],
        [ '1e23',                     '44b52d02c7e14af6' ],
        [ '2.2250738585072014e-308',  '0010000000000000' ], # min normal
        [ '2.2250738585072011e-308',  '000fffffffffffff' ], # max subnormal
        [ '4.9406564584124654e-324',  '0000000000000001' ], # min subnormal
        [ '2.4703282292062327e-324',  '0000000000000000' ], # half min subnormal -> 0
        [ '2.4703282292062328e-324',  '0000000000000001' ], # > half min subnormal
        [ '1.7976931348623157e+308',  '7fefffffffffffff' ], # max normal
        [ '1.7976931348623159e+308',  '7ff0000000000000' ], # overflow -> +Inf
        # Exact half-ULP ties (round to nearest even)
        [ '0.500000000000000055511151231257827021181583404541015625',          '3fe0000000000000' ],
        [ '0.50000000000000005551115123125782702118158340454101562500000001',  '3fe0000000000001' ],
        [ '9007199254740992.5',       '4340000000000000' ], # 2^53 + 0.5 -> 2^53
        [ '9007199254740993.5',       '4340000000000001' ], # 2^53 + 1.5 -> 2^53 + 2
        # Long-digit inputs (>30 digits)
        [ '3.14159265358979323846264338327950288419716939937510', '400921fb54442d18' ],
        [ '2.71828182845904523536028747135266249775724709369995', '4005bf0a8b145769' ],
    );

    my @parse_rows;
    for my $tc (@parse_cases) {
        my ($str, $want) = @$tc;
        my $val = $str * 1.0;
        my $got = dhex($val);
        is($got, $want, "parse '$str' -> $want");
        push @parse_rows, "parse:$str:$got";
    }

    # Perl compatibility: "0x10" + 0 stops at '0'
    {
        no warnings 'numeric';
        is("0x10" + 0, 0, '0x10 numeric conversion stops at 0');
        is("0b10" + 0, 0, '0b10 numeric conversion stops at 0');
    }

    my $d = digest_rows(@parse_rows);
    note("parse_digest=$d");
    push @all_rows, @parse_rows;
}

# =========================================================================
# 3. Formatting (default %.15g + PERL_NV_SHORTEST=1 Ryu)
# =========================================================================
{
    # Default formatting must remain %.15g
    delete local $ENV{PERL_NV_SHORTEST};
    my $v13 = 1.0 / 3.0;
    is("$v13", '0.333333333333333', 'default NV stringification unchanged (%.15g)');

    # sprintf formats
    my @fmt_rows;
    for my $v (0.1, 1.0/3.0, 3.141592653589793, 1e-5, 1e20) {
        push @fmt_rows, sprintf('g:%.15g|e:%.10e|f:%.6f', $v, $v, $v);
    }

    # Enable PERL_NV_SHORTEST=1 for Ryu shortest round-trip
    local $ENV{PERL_NV_SHORTEST} = '1';
    my @ryu_spots = (
        [ 0.1,                      '0.1' ],
        [ 0.3,                      '0.3' ],
        [ 1.0 / 3.0,                '0.3333333333333333' ],
        [ 1e23,                     '1e+23' ],
        [ 4.9406564584124654e-324,  '5e-324' ],
        [ 1.7976931348623157e+308,  '1.7976931348623157e+308' ],
        [ 2.2250738585072014e-308,  '2.2250738585072014e-308' ],
    );
    for my $tc (@ryu_spots) {
        my ($val, $want) = @$tc;
        # Force fresh NV -> PV conversion
        my $copy = $val + 0.0;
        my $got = "$copy";
        is($got, $want, "PERL_NV_SHORTEST=1 formats $want");
        push @fmt_rows, "ryu:$got";
    }

    # Deterministic 2,000-double round-trip test under PERL_NV_SHORTEST=1
    my $rng_hi = 0x12345678;
    my $rng_lo = 0x9abcdef1;
    my $rt_fails = 0;
    for (1 .. 2000) {
        $rng_hi = (($rng_hi * 1103515245 + 12345) & 0xffffffff);
        $rng_lo = (($rng_lo * 1664525 + 1013904223) & 0xffffffff);
        my $exp = ($rng_hi >> 20) & 0x7ff;
        next if $exp == 0x7ff;
        my $raw = pack('NN', $rng_hi, $rng_lo);
        my $nv = unpack('d>', $raw);
        my $s = "$nv";
        my $back = $s + 0.0;
        if (pack('d>', $back) ne $raw) {
            $rt_fails++;
            diag("round-trip mismatch: orig=" . unpack('H16', $raw) . " str='$s' back=" . dhex($back))
                if $rt_fails <= 5;
        }
    }
    is($rt_fails, 0, '2000 deterministic doubles round-trip bit-identically with PERL_NV_SHORTEST=1');

    my $d = digest_rows(@fmt_rows);
    note("fmt_digest=$d");
    push @all_rows, @fmt_rows;
}

# =========================================================================
# 4. NaN canonicalisation
# =========================================================================
{
    my $inf = "Inf" + 0.0;
    my $ninf = "-Inf" + 0.0;
    my $nan_str = "NaN" + 0.0;
    my $nnan_str = "-NaN" + 0.0;

    my @nans = (
        [ '0 * Inf',     0.0 * $inf ],
        [ '0 * -Inf',    0.0 * $ninf ],
        [ 'Inf - Inf',   $inf - $inf ],
        [ 'Inf + -Inf',  $inf + $ninf ],
        [ 'Inf / Inf',   $inf / $inf ],
        [ '(-1)**0.5',   (-1.0) ** 0.5 ],
        [ 'sin(Inf)',    sin($inf) ],
        [ 'cos(-Inf)',   cos($ninf) ],
        [ 'atan2(NaN,1)',atan2($nan_str, 1.0) ],
        [ '"NaN"+0',     $nan_str ],
        [ '"-NaN"+0',    $nnan_str ],
        [ '-(0*Inf)',    -(0.0 * $inf) ],
        [ 'abs(0*Inf)',  abs(0.0 * $inf) ],
        [ 'int(0*Inf)',  int(0.0 * $inf) ],
        [ 'NaN + 1',     $nan_str + 1.0 ],
        [ 'NaN - 1',     $nan_str - 1.0 ],
        [ 'NaN * 2',     $nan_str * 2.0 ],
        [ 'NaN / 2',     $nan_str / 2.0 ],
    );

    my @nan_rows;
    for my $tc (@nans) {
        my ($label, $v) = @$tc;
        my $h = dhex($v);
        is($h, '7ff8000000000000', "canonical qNaN for $label");
        is("$v", 'NaN', "canonical NaN stringifies as NaN for $label");
        push @nan_rows, "nan:$label:$h";
    }
    my $d = digest_rows(@nan_rows);
    note("nan_digest=$d");
    push @all_rows, @nan_rows;
}

# =========================================================================
# 5. Correctly-rounded binary64 libm
# =========================================================================
{
    my @libm_spots = (
        [ 'sin(1.0)',           sin(1.0),                 '3feaed548f090cee' ],
        [ 'cos(1.0)',           cos(1.0),                 '3fe14a280fb5068c' ],
        [ 'sin(1e20)',          sin(1e20),                'bfe4a5e605fd6450' ],
        [ 'cos(1e20)',          cos(1e20),                '3fe872720fc60d3d' ],
        [ 'exp(1.0)',           exp(1.0),                 '4005bf0a8b145769' ],
        [ 'log(2.0)',           log(2.0),                 '3fe62e42fefa39ef' ],
        [ 'sqrt(2.0)',          sqrt(2.0),                '3ff6a09e667f3bcd' ],
        [ 'atan2(1.0, 1.0)',    atan2(1.0, 1.0),          '3fe921fb54442d18' ],
        [ 'atan2(-0.0, -1.0)',  atan2(-0.0, -1.0),        'c00921fb54442d18' ],
        [ '2.0 ** 0.5',         2.0 ** 0.5,               '3ff6a09e667f3bcd' ],
        [ '3.0 ** 2.5',         3.0 ** 2.5,               '402f2d4a45635640' ],
    );

    my @libm_rows;
    for my $tc (@libm_spots) {
        my ($label, $val, $want) = @$tc;
        my $got = dhex($val);
        is($got, $want, "CR libm $label -> $want");
        push @libm_rows, "libm:$label:$got";
    }

    # Additional corpus rows for cross-host digest
    for my $x (0.1, 0.5, 1.5, 2.5, 10.0, 100.0, 1e6, 1e100) {
        push @libm_rows, "sin:$x:" . dhex(sin($x));
        push @libm_rows, "cos:$x:" . dhex(cos($x));
        push @libm_rows, "log:$x:" . dhex(log($x));
        push @libm_rows, "sqrt:$x:" . dhex(sqrt($x));
        push @libm_rows, "exp:$x:" . dhex(exp($x)) if $x <= 100.0;
    }
    push @libm_rows, "pow_hard:" . dhex(1.0000001 ** 1000000.0);

    my $d = digest_rows(@libm_rows);
    note("libm_digest=$d");
    push @all_rows, @libm_rows;
}

# =========================================================================
# 6. MT::Decimal (IEEE 754-2008 decimal128)
# =========================================================================
{
    my @dec_rows;

    my $d01 = MT::Decimal->new('0.1');
    my $d02 = MT::Decimal->new('0.2');
    my $d03 = $d01 + $d02;
    is("$d03", '0.3', 'MT::Decimal 0.1 + 0.2 eq 0.3');
    ok($d03 == MT::Decimal->new('0.3'), 'MT::Decimal 0.1 + 0.2 == 0.3');
    push @dec_rows, "d03:" . unpack('H32', $d03->to_bytes);

    # Money-style invoice test (exact to cents)
    my $subtotal = MT::Decimal->new('19.99') * 3 + MT::Decimal->new('4.50') * 2;
    my $tax = ($subtotal * MT::Decimal->new('0.0825'))->quantize(-2, MT::Decimal::ROUND_HALF_UP);
    my $total = $subtotal + $tax;
    is("$subtotal", '68.97', 'money subtotal exact');
    is("$tax", '5.69', 'money tax HALF_UP exact');
    is("$total", '74.66', 'money total exact');
    push @dec_rows, "money:$subtotal:$tax:$total:" . unpack('H32', $total->to_bytes);

    # 34-digit precision
    my $one = MT::Decimal->new('1');
    my $three = MT::Decimal->new('3');
    my $third = $one / $three;
    is("$third", '0.3333333333333333333333333333333333', '1/3 to 34 decimal digits');
    push @dec_rows, "third:" . unpack('H32', $third->to_bytes);

    # Rounding modes
    is(MT::Decimal->new('2.5')->quantize(0, MT::Decimal::ROUND_HALF_EVEN)->to_string, '2', 'ROUND_HALF_EVEN 2.5 -> 2');
    is(MT::Decimal->new('3.5')->quantize(0, MT::Decimal::ROUND_HALF_EVEN)->to_string, '4', 'ROUND_HALF_EVEN 3.5 -> 4');
    is(MT::Decimal->new('2.5')->quantize(0, MT::Decimal::ROUND_HALF_UP)->to_string,   '3', 'ROUND_HALF_UP 2.5 -> 3');
    is(MT::Decimal->new('2.9')->quantize(0, MT::Decimal::ROUND_DOWN)->to_string,      '2', 'ROUND_DOWN 2.9 -> 2');
    is(MT::Decimal->new('-2.1')->quantize(0, MT::Decimal::ROUND_FLOOR)->to_string,   '-3', 'ROUND_FLOOR -2.1 -> -3');
    is(MT::Decimal->new('2.1')->quantize(0, MT::Decimal::ROUND_CEIL)->to_string,      '3', 'ROUND_CEIL 2.1 -> 3');

    my $d = digest_rows(@dec_rows);
    note("decimal_digest=$d");
    push @all_rows, @dec_rows;
}

my $overall = digest_rows(@all_rows);
note("overall_numeric_digest=$overall");
ok(length($overall) == 64, "overall numeric SHA-256 digest computed ($overall)");

done_testing();

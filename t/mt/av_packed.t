#!./perl
use strict;
use warnings;
use Test::More;
use Math::BigFloat;
use Storable qw(freeze thaw);
use MT::Array;

sub dhex { unpack("H*", pack("d", $_[0])) }

# Child mode for SIMD vs scalar bit-for-bit parity check
if (@ARGV && $ARGV[0] eq '--parity') {
    my $seed = 0x51a7c0de;
    my $rng = sub {
        $seed = ($seed * 1103515245 + 12345) & 0x7fffffff;
        return $seed;
    };

    my @nv_vals;
    for my $i (1 .. 500) {
        my $e = ($rng->() % 40) - 20;
        my $m = (($rng->() % 2000001) - 1000000) / 1000000.0;
        push @nv_vals, $m * (2.0 ** $e);
    }
    push @nv_vals, (1e16, 1.0, -1e16, 1e-16, -1e-16, 0.5, 2**-53);

    my $a = MT::Array::packed('NV', @nv_vals);
    my $b = MT::Array::packed('NV', reverse @nv_vals);
    my $s = MT::Array::sum($a);
    my $d = MT::Array::dot($a, $b);
    my $mn = MT::Array::min($a);
    my $mx = MT::Array::max($a);
    my $ma = MT::Array::map_add($a, 3.25);
    my $mas = MT::Array::sum($ma);

    my $u8 = MT::Array::packed('U8', map { $_ & 255 } 0 .. 4095);
    my $cnt = MT::Array::count($u8, 42);
    my $fnd = MT::Array::find($u8, 255);

    my $iv = MT::Array::packed('IV', map { ($rng->() % 100000) - 50000 } 1 .. 1000);
    my $ivs = MT::Array::sum($iv);
    my $ivmn = MT::Array::min($iv);
    my $ivmx = MT::Array::max($iv);

    print join(" ",
        "PARITY",
        dhex($s), dhex($d), dhex($mn), dhex($mx), dhex($mas),
        $cnt, $fnd, $ivs, $ivmn, $ivmx
    ) . "\n";
    exit 0;
}

# 1. Constructors, pack/unpack, is_packed
{
    my $iv = MT::Array::packed('IV', 10, -20, 30, 40);
    is(MT::Array::is_packed($iv), 'IV', "packed IV is_packed");
    is(scalar(@$iv), 4, "packed IV length");
    is($iv->[0], 10, "packed IV fetch 0");
    is($iv->[1], -20, "packed IV fetch 1");
    is($iv->[-1], 40, "packed IV negative fetch");
    is(MT::Array::is_packed($iv), 'IV', "still packed after fetches");

    $iv->[1] = 99;
    is($iv->[1], 99, "packed IV lvalue store");
    is(MT::Array::is_packed($iv), 'IV', "still packed after fitting IV store");

    push @$iv, 50;
    is(scalar(@$iv), 5, "push length");
    is(pop(@$iv), 50, "pop value");
    is(shift(@$iv), 10, "shift value");
    is(scalar(@$iv), 3, "length after shift");
    is(MT::Array::is_packed($iv), 'IV', "still packed after push/pop/shift");

    my @plain = (1.5, -2.5, 3.25);
    ok(!MT::Array::is_packed(\@plain), "plain array not packed");
    MT::Array::pack(\@plain, 'NV');
    is(MT::Array::is_packed(\@plain), 'NV', "packed in place as NV");
    is($plain[1], -2.5, "NV element intact after pack");

    my $u8 = MT::Array::packed('U8', 0, 1, 42, 255);
    is(MT::Array::is_packed($u8), 'U8', "packed U8");
    is($u8->[3], 255, "U8 fetch 255");
    eval { MT::Array::packed('U8', 256) };
    like($@, qr/does not fit/, "U8 rejects 256 in constructor");
}

# 2. Store of non-fitting value unpacks permanently
{
    my $before = MT::Array::unpack_count();
    my $iv = MT::Array::packed('IV', 1, 2, 3);
    $iv->[1] = "hello";
    ok(!MT::Array::is_packed($iv), "storing string into IV unpacks permanently");
    is($iv->[1], "hello", "stored string preserved");
    is($iv->[0], 1, "sibling IV preserved");
    ok(MT::Array::unpack_count() > $before, "unpack_count incremented");

    my $u8 = MT::Array::packed('U8', 10, 20, 30);
    $u8->[0] = 300;
    ok(!MT::Array::is_packed($u8), "storing 300 into U8 unpacks permanently");
    is($u8->[0], 300, "300 preserved after U8 unpack");
}

# 3. Every raw AvARRAY unpack trigger unpacks permanently with identical results
{
    # splice
    my $arr_sp = MT::Array::packed('IV', 1, 2, 3, 4, 5);
    my $c0 = MT::Array::unpack_count();
    my @removed = splice(@$arr_sp, 1, 2, 20, 30);
    ok(!MT::Array::is_packed($arr_sp), "splice unpacks permanently");
    ok(MT::Array::unpack_count() > $c0, "splice incremented unpack_count");
    is_deeply(\@removed, [2, 3], "splice removed elements match");
    is_deeply($arr_sp, [1, 20, 30, 4, 5], "splice result matches");

    # sort
    my $arr_so = MT::Array::packed('IV', 5, 1, 4, 2, 3);
    my $c1 = MT::Array::unpack_count();
    my @sorted = sort { $a <=> $b } @$arr_so;
    ok(!MT::Array::is_packed($arr_so), "sort unpacks permanently");
    ok(MT::Array::unpack_count() > $c1, "sort incremented unpack_count");
    is_deeply(\@sorted, [1, 2, 3, 4, 5], "sort result matches");

    # list assignment
    my $arr_la = MT::Array::packed('IV', 7, 8, 9);
    my $c2 = MT::Array::unpack_count();
    my @copy = @$arr_la;
    ok(!MT::Array::is_packed($arr_la), "list assignment unpacks permanently");
    ok(MT::Array::unpack_count() > $c2, "list assignment incremented unpack_count");
    is_deeply(\@copy, [7, 8, 9], "list assignment result matches");

    # Storable round trip (works identically whether unpacked or via av_fetch)
    my $arr_st = MT::Array::packed('NV', 1.25, 2.5, 3.75);
    my $thawed = thaw(freeze($arr_st));
    is_deeply($thawed, [1.25, 2.5, 3.75], "Storable round trip matches");

    # local element & local @a
    our @loc_arr;
    @loc_arr = (10, 20, 30);
    MT::Array::pack(\@loc_arr, 'IV');
    is(MT::Array::is_packed(\@loc_arr), 'IV', "package array packed before local");
    {
        local $loc_arr[1] = 99;
        is($loc_arr[1], 99, "local element override works");
    }
    is($loc_arr[1], 20, "local element restored");
    ok(!MT::Array::is_packed(\@loc_arr), "local element unpacked permanently");

    # dump.c PACKED_AV flag name check via Devel::Peek
    my $inc = -f 'lib/strict.pm' ? '-I. -Ilib' : '-I.. -I../lib';
    my $dump_out = `$^X $inc -MDevel::Peek -MMT::Array -e 'my \$x = MT::Array::packed("IV", 1, 2); Dump(\$x)' 2>&1`;
    like($dump_out, qr/PACKED_AV/, "Devel::Peek::Dump prints PACKED_AV flag");
    unlike($dump_out, qr/SV = PVAV[^\n]*UTF8/, "Devel::Peek::Dump does not print UTF8 on PVAV");
}

# 4. Random ops: packed vs plain digest comparison
{
    my $seed = 0x13579bdf;
    my $rng = sub {
        $seed = ($seed * 1103515245 + 12345) & 0x7fffffff;
        return $seed;
    };

    my $p = MT::Array::packed('IV', 1 .. 100);
    my $q = [1 .. 100];
    my ($dig_p, $dig_q) = (0, 0);

    for my $i (1 .. 50_000) {
        my $op = $rng->() % 6;
        if ($op == 0) {
            my $v = ($rng->() % 10000) - 5000;
            push @$p, $v;
            push @$q, $v;
        } elsif ($op == 1 && @$p > 10) {
            my $vp = pop @$p;
            my $vq = pop @$q;
            $dig_p = ($dig_p * 31 + $vp) & 0x7fffffff;
            $dig_q = ($dig_q * 31 + $vq) & 0x7fffffff;
        } elsif ($op == 2 && @$p > 10) {
            my $vp = shift @$p;
            my $vq = shift @$q;
            $dig_p = ($dig_p * 31 + $vp) & 0x7fffffff;
            $dig_q = ($dig_q * 31 + $vq) & 0x7fffffff;
        } elsif ($op == 3) {
            my $idx = $rng->() % scalar(@$p);
            my $v = ($rng->() % 10000) - 5000;
            $p->[$idx] = $v;
            $q->[$idx] = $v;
        } else {
            my $idx = $rng->() % scalar(@$p);
            $dig_p = ($dig_p * 31 + $p->[$idx]) & 0x7fffffff;
            $dig_q = ($dig_q * 31 + $q->[$idx]) & 0x7fffffff;
        }
    }
    is(MT::Array::is_packed($p), 'IV', "stayed packed across 50k random ops");
    is($dig_p, $dig_q, "packed vs plain digest identical over 50k random ops");
    is(MT::Array::sum($p), MT::Array::sum($q), "packed vs plain sum identical");
    is(MT::Array::min($p), MT::Array::min($q), "packed vs plain min identical");
    is(MT::Array::max($p), MT::Array::max($q), "packed vs plain max identical");
}

# 5. IV 128-bit exact accumulation (overflow-to-NV applied once)
{
    my $iv_max = (~0 >> 1);
    my $a = MT::Array::packed('IV', $iv_max, $iv_max, -$iv_max);
    is(MT::Array::sum($a), $iv_max, "IV 128-bit exact sum cancels intermediate overflow");

    my $b = MT::Array::packed('IV', $iv_max, $iv_max, $iv_max);
    my $sb = MT::Array::sum($b);
    ok($sb > $iv_max * 2.5, "IV 128-bit sum overflows cleanly to NV when final sum exceeds UV_MAX");
}

# 6. NV correctly-rounded sum & dot vs Math::BigFloat (incl. cancellation)
{
    # Catastrophic cancellation
    my $cat = MT::Array::packed('NV', 1e16, 1.0, -1e16, 1e-16);
    is(dhex(MT::Array::sum($cat)), dhex(1.0 + 1e-16),
       "NV sum exact under catastrophic cancellation (1e16 + 1 - 1e16 + 1e-16)");

    # Exact half-ulp tie breaking
    my $tie_up = MT::Array::packed('NV', 1.0, 2.0**-53, 2.0**-100);
    is(dhex(MT::Array::sum($tie_up)), dhex(1.0 + 2.0**-52),
       "NV sum rounds half-ulp + eps UP");

    my $tie_dn = MT::Array::packed('NV', 1.0, 2.0**-53, -(2.0**-100));
    is(dhex(MT::Array::sum($tie_dn)), dhex(1.0),
       "NV sum rounds half-ulp - eps DOWN");

    # 100 random NV arrays vs Math::BigFloat exact reference
    my $seed = 0x2468ace1;
    my $rng = sub {
        $seed = ($seed * 1103515245 + 12345) & 0x7fffffff;
        return $seed;
    };
    my $all_exact = 1;
    for my $trial (1 .. 100) {
        my $n = 8 + ($rng->() % 24);
        my @xs;
        my $bf = Math::BigFloat->new(0);
        $bf->accuracy(120);
        for my $i (1 .. $n) {
            my $e = ($rng->() % 30) - 15;
            my $m = (($rng->() % 2001) - 1000) / 1024.0;
            my $v = $m * (2.0 ** $e);
            push @xs, $v;
            $bf->badd(Math::BigFloat->new(sprintf("%.17g", $v)));
        }
        # Inject cancellation pair
        push @xs, 1e12, -1e12;
        my $p = MT::Array::packed('NV', @xs);
        my $got = MT::Array::sum($p);
        my $exp = 0.0 + $bf->bstr();
        if (dhex($got) ne dhex($exp)) {
            diag("trial $trial mismatch: got=$got (".dhex($got).") exp=$exp (".dhex($exp).")");
            $all_exact = 0;
            last;
        }
    }
    ok($all_exact, "100 random NV arrays (incl. cancellation) match Math::BigFloat bit-for-bit");

    # NaN propagation in min/max
    my $nan = 9**9**9 - 9**9**9;
    my $wnan = MT::Array::packed('NV', 1.0, 2.0, $nan, 3.0);
    ok(MT::Array::min($wnan) != MT::Array::min($wnan), "min propagates NaN");
    ok(MT::Array::max($wnan) != MT::Array::max($wnan), "max propagates NaN");
}

# 7. U8 count / find / slice / map_add
{
    my @bytes = map { $_ & 255 } 0 .. 1023;
    my $u8 = MT::Array::packed('U8', @bytes);
    is(MT::Array::count($u8, 42), 4, "U8 count(42) == 4");
    is(MT::Array::find($u8, 200), 200, "U8 find(200) == 200");
    is(MT::Array::find($u8, 0), 0, "U8 find(0) == 0");

    my $sl = MT::Array::slice($u8, 10, 5);
    is(MT::Array::is_packed($sl), 'U8', "slice preserves U8 packed type");
    is_deeply([@$sl], [10, 11, 12, 13, 14], "slice contents match");

    my $iv = MT::Array::packed('IV', 1, 2, 3, 4, 5);
    my $iv2 = MT::Array::map_add($iv, 10);
    is(MT::Array::is_packed($iv2), 'IV', "map_add preserves IV packed type");
    is(MT::Array::sum($iv2), 65, "map_add sum exact");
}

# 8. SIMD vs PERL_FORCE_SCALAR=1 bit-for-bit parity
{
    my $inc = -f 'lib/strict.pm' ? '-I. -Ilib' : '-I.. -I../lib';
    my $d0 = do { local $ENV{PERL_FORCE_SCALAR} = 0; `$^X $inc $0 --parity` };
    die "parity SIMD child failed ($?)" if $? != 0;
    my $d1 = do { local $ENV{PERL_FORCE_SCALAR} = 1; `$^X $inc $0 --parity` };
    die "parity scalar child failed ($?)" if $? != 0;
    chomp($d0, $d1);
    like($d0, qr/^PARITY /, "parity SIMD output valid");
    is($d0, $d1, "SIMD and PERL_FORCE_SCALAR=1 agree bit-for-bit on all MT::Array ops");
}

# 9. Thread-safety in MT::Pool (1 worker and 16 workers, clean child processes)
{
    for my $nw (1, 16) {
        my $code = <<'END_CODE';
use strict; use warnings;
use MT::Array; use MT::Pool;
sub pool_sum_job {
    my $n = shift;
    my $a = MT::Array::packed("IV", 1 .. ($n * 10));
    return MT::Array::sum($a);
}
my $nw = shift @ARGV;
my $pool = MT::Pool->new(workers => $nw);
my @jobs = map { "$_" } 1 .. 32;
my @res = $pool->map(\&pool_sum_job, @jobs);
$pool->shutdown;
for my $i (0 .. $#jobs) {
    my $m = $jobs[$i] * 10;
    my $exp = $m * ($m + 1) / 2;
    die "mismatch at $i: got $res[$i] expected $exp" if $res[$i] != $exp;
}
print "POOL_OK $nw\n";
END_CODE
        my $inc = -f 'lib/strict.pm' ? '-I. -Ilib' : '-I.. -I../lib';
        my $out = `$^X $inc -e '$code' $nw`;
        is($?, 0, "MT::Pool ($nw workers) child exited 0");
        chomp $out;
        is($out, "POOL_OK $nw", "MT::Array::sum exact across $nw workers in MT::Pool");
    }
}

done_testing();

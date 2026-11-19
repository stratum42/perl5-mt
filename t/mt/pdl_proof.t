#!./perl
use strict;
use warnings;
use Test::More;
use Digest::SHA qw(sha256_hex);

BEGIN {
    chdir 't' if -d 't' && ! -d 'mt';
    unshift @INC, '../lib', 'lib', '.';
}

use MT;
use MT::Pool;
use MT::PDLBridge;

sub pdl_worker_slice {
    my ($spec) = @_;
    my ($src_id, $dst_id, $off, $len, $a, $b) = split(/,/, $spec);
    return MT::PDLBridge::worker_slice_kernel($src_id, $dst_id, $off, $len, $a, $b);
}

# 1. Threadsafe marker & per-interpreter audit report
is(MT::PDLBridge::is_threadsafe(), 1, '10f: MT::PDLBridge reports PERL_XS_THREADSAFE');
my $audit = MT::PDLBridge::audit_report();
is($audit->{xs_threadsafe_marker}, 'PERL_XS_THREADSAFE', '10f: audit reports threadsafe marker');
ok(scalar(@{ $audit->{per_interpreter_audit} }) >= 3, '10f: audit documents Core table, bad-values, and shm storage');
ok(scalar(@{ $audit->{compartment_breaks} }) >= 1, '10f: audit records compartment breakage causes');

# 2. SHM-backed ndarray lifecycle & sealing
my $arr = MT::PDLBridge->create_array(nelem => 16, dims => [4, 4], dtype => 'f64');
ok($arr && $arr->id > 0, '10f: allocated shm-backed ndarray');
is_deeply($arr->info->{dims}, [4, 4], '10f: dims metadata preserved');
$arr->fill_seq(1.0, 0.5);
is($arr->get(0), 1.0, '10f: element 0 initialized');
is($arr->get(15), 8.5, '10f: element 15 initialized');
is($arr->is_sealed, 0, '10f: ndarray initially writable');
$arr->seal;
is($arr->is_sealed, 1, '10f: ndarray sealed read-only');
eval { $arr->set(0, 99.0) };
like($@, qr/failed/i, '10f: writes to sealed ndarray rejected');
$arr->destroy;

# 3. Multi-worker sliced computation with bit-exact SHA-256 parity
MT::Image::freeze() unless MT::Image::frozen();

my $nelem   = 8192;
my $workers = 4;
my $start   = 0.125;
my $step    = 0.00025;
my $alpha   = 1.375;
my $beta    = -0.625;

my $ref = MT::PDLBridge::scalar_reference_kernel($start, $step, $nelem, $alpha, $beta);

my $src = MT::PDLBridge->create_array(nelem => $nelem, dims => [$nelem], dtype => 'f64');
my $dst = MT::PDLBridge->create_array(nelem => $nelem, dims => [$nelem], dtype => 'f64');
$src->fill_seq($start, $step);
$src->seal;

my $pool  = MT::Pool->new(workers => $workers);
my $chunk = int($nelem / $workers);
my @specs;
for my $w (0 .. $workers - 1) {
    my $off = $w * $chunk;
    my $len = ($w == $workers - 1) ? ($nelem - $off) : $chunk;
    push @specs, join(',', $src->id, $dst->id, $off, $len, $alpha, $beta);
}

my @slice_res = $pool->map(\&pdl_worker_slice, @specs);
is(scalar(@slice_res), $workers, '10f: all worker slices completed');

$dst->seal;
my $mt_digest = $dst->digest;
is($mt_digest, $ref->{digest},
   '10f: multi-worker shm ndarray SHA-256 matches single-threaded scalar reference bit-for-bit');

my $mt_sum = $dst->sum_slice(0, $nelem);
is(unpack('H16', pack('d>', $mt_sum)), unpack('H16', pack('d>', $ref->{sum})),
   '10f: compensated reduction sum matches scalar reference bit-for-bit');

# 4. Prove every worker computes identical bits for the same slice
my $dup_a = MT::PDLBridge->create_array(nelem => 512, dtype => 'f64');
my $dup_b = MT::PDLBridge->create_array(nelem => 512, dtype => 'f64');
my @dup_res = $pool->map(\&pdl_worker_slice,
    join(',', $src->id, $dup_a->id, 0, 512, $alpha, $beta),
    join(',', $src->id, $dup_b->id, 0, 512, $alpha, $beta),
);
my (undef, undef, $dig_a, $sum_a) = split(/\|/, $dup_res[0]);
my (undef, undef, $dig_b, $sum_b) = split(/\|/, $dup_res[1]);
is($dig_a, $dig_b, '10f: two workers computing identical slice produce identical SHA-256');
is($sum_a, $sum_b, '10f: two workers computing identical slice produce identical sum bits');

# Dot product check
my $dot = $src->dot_slice($dst, 0, 64);
ok($dot != 0.0, '10f: dot_slice produces non-zero deterministic reduction');

# Optional CPAN PDL auto-bridge check
if (MT::PDLBridge::has_pdl()) {
    my $p = PDL::sequence(PDL::double(), 32);
    my $b = MT::PDLBridge->from_pdl($p);
    my $p2 = MT::PDLBridge->to_pdl($b);
    is($p2->nelem, 32, '10f: CPAN PDL round-trip bridge preserves element count');
    $b->destroy;
} else {
    pass('10f: CPAN PDL not installed; standalone shm bridge verified cleanly');
}

$pool->shutdown;
$dup_a->destroy;
$dup_b->destroy;
$src->destroy;
$dst->destroy;

done_testing();

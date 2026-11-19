#!/usr/bin/env perl
#
# tools/pdl-proof.pl - PDL threadsafe proof and benchmark
#
# Proves that a shm-backed ndarray sliced across MT::Pool workers produces
# a bit-exact SHA-256 digest matching single-threaded scalar IEEE-754
# computation, and auto-benchmarks against CPAN PDL if installed.

use strict;
use warnings;
use Time::HiRes qw(time);
use Digest::SHA qw(sha256_hex);

use MT;
use MT::Pool;
use MT::PDLBridge;

my $nelem   = $ENV{PDL_PROOF_NELEM}   || 65536;
my $workers = $ENV{PDL_PROOF_WORKERS} || 4;
my $start   = 0.125;
my $step    = 0.00025;
my $alpha   = 1.375;
my $beta    = -0.625;

sub _run_slice {
    my ($spec) = @_;
    my ($src_id, $dst_id, $off, $len, $a, $b) = split(/,/, $spec);
    return MT::PDLBridge::worker_slice_kernel($src_id, $dst_id, $off, $len, $a, $b);
}

MT::Image::freeze() unless MT::Image::frozen();

my $audit = MT::PDLBridge::audit_report();
printf "MT::PDLBridge Proof\n";
printf "  threadsafe_marker : %s (is_threadsafe=%d)\n",
       $audit->{xs_threadsafe_marker}, MT::PDLBridge::is_threadsafe();
printf "  cpan_pdl_installed: %s\n", $audit->{pdl_installed} ? 'yes' : 'no';
printf "  nelem=%d workers=%d\n", $nelem, $workers;

# 1. Single-threaded scalar reference
my $t0 = time();
my $ref = MT::PDLBridge::scalar_reference_kernel($start, $step, $nelem, $alpha, $beta);
my $t_scalar = time() - $t0;

# 2. Multi-worker shm-backed ndarray sliced across MT::Pool
my $src = MT::PDLBridge->create_array(nelem => $nelem, dims => [$nelem], dtype => 'f64');
my $dst = MT::PDLBridge->create_array(nelem => $nelem, dims => [$nelem], dtype => 'f64');
$src->fill_seq($start, $step);
$src->seal();

my $pool = MT::Pool->new(workers => $workers);
my $chunk = int(($nelem + $workers - 1) / $workers);
my @specs;
for (my $off = 0; $off < $nelem; $off += $chunk) {
    my $len = ($off + $chunk > $nelem) ? ($nelem - $off) : $chunk;
    push @specs, join(',', $src->id, $dst->id, $off, $len, $alpha, $beta);
}

my $t1 = time();
my @results = $pool->map(\&_run_slice, @specs);
my $t_mt = time() - $t1;

$dst->seal();
my $mt_digest = $dst->digest();
my $mt_sum    = $dst->sum_slice(0, $nelem);

# Verify parity
my $parity_ok = ($mt_digest eq $ref->{digest}) ? 1 : 0;
printf "  scalar_sha256     : %s (%.4fs)\n", $ref->{digest}, $t_scalar;
printf "  worker_sha256     : %s (%.4fs)\n", $mt_digest, $t_mt;
printf "  scalar_sum_hex    : %s\n", unpack('H16', pack('d>', $ref->{sum}));
printf "  worker_sum_hex    : %s\n", unpack('H16', pack('d>', $mt_sum));
printf "  digest_parity     : %s\n", $parity_ok ? 'PASS' : 'FAIL';

# Optional stock CPAN PDL benchmark when installed
if ($audit->{pdl_installed}) {
    my $tp0 = time();
    my $p = PDL::sequence(PDL::double(), $nelem) * $step + $start;
    my $q = (($p * $alpha) + $beta) * ($p + 0.5);
    my $t_pdl = time() - $tp0;
    my $bridge = MT::PDLBridge->from_pdl($q);
    my $pdl_digest = $bridge->digest();
    printf "  stock_pdl_sha256  : %s (%.4fs, parity=%s)\n",
           $pdl_digest, $t_pdl, ($pdl_digest eq $ref->{digest} ? 'PASS' : 'DIFF');
    $bridge->destroy();
}

$pool->shutdown();
$src->destroy();
$dst->destroy();

exit($parity_ok ? 0 : 1);

#!/usr/bin/perl
# perl5-mt: reworked perl5-mt pool benchmark driver.
#
# Supports:
#   - Large preloaded image before freeze (--image-mb / PERL_BENCH_IMAGE_MB)
#   - In-process (PERL_XS_THREADSAFE) and compartment JSON modes (--json-mode)
#   - Per-worker fiber concurrency (--fibers N; when N > 1, each worker runs N
#     scheduled MT::Fiber coroutines over :async I/O)
#   - Reports base_rss_kb and rss_per_worker_kb alongside ops_per_s and startup_ms.
#
# Usage:
#   perl mt.pl [workers] [items] [libdir]
#   perl mt.pl --workers N --items M --fibers F --image-mb MB --json-mode MODE [--lib DIR]

use strict;
use warnings;
use Time::HiRes qw(time);
use FindBin;
use lib "$FindBin::Bin";
use Workload qw(setup item rss_kb rss_per_worker_kb json_mode_name image_mb);
use MT::Pool;
use MT::Fiber;

sub job { return Workload::item($_[0]) }

sub fiber_batch {
    my ($spec) = @_;
    my ($start_i, $count, $nfibers) = split /:/, $spec;
    $nfibers = 1 if !$nfibers || $nfibers < 1;
    my $per = int(($count + $nfibers - 1) / $nfibers);
    my @fids;
    for my $f (0 .. $nfibers - 1) {
        my $lo = $start_i + $f * $per;
        my $hi = $lo + $per - 1;
        $hi = $start_i + $count - 1 if $hi >= $start_i + $count;
        next if $lo > $hi;
        push @fids, MT::Fiber::schedule(sub {
            my $acc = 0;
            for my $i ($lo .. $hi) {
                $acc += Workload::item($i);
                MT::Fiber::pass() if ($i & 3) == 0;
            }
            return $acc;
        });
    }
    MT::Fiber::run();
    my $total = 0;
    $total += MT::Fiber::join($_) for @fids;
    return $total;
}

my $n         = 4;
my $items     = 2000;
my $fibers    = $ENV{PERL_BENCH_FIBERS}    // 1;
my $img_mb    = $ENV{PERL_BENCH_IMAGE_MB}  // 0;
my $json_mode = $ENV{PERL_BENCH_JSON_MODE} // 'auto';
my $lib;

if (@ARGV && $ARGV[0] =~ /^--/) {
    while (@ARGV) {
        my $opt = shift @ARGV;
        if    ($opt eq '--workers')   { $n         = shift @ARGV }
        elsif ($opt eq '--items')     { $items     = shift @ARGV }
        elsif ($opt eq '--fibers')    { $fibers    = shift @ARGV }
        elsif ($opt eq '--image-mb')  { $img_mb    = shift @ARGV }
        elsif ($opt eq '--json-mode') { $json_mode = shift @ARGV }
        elsif ($opt eq '--lib')       { $lib       = shift @ARGV }
        else { die "Unknown option: $opt\n" }
    }
}
else {
    $n     = shift @ARGV if @ARGV && defined $ARGV[0] && length $ARGV[0];
    $items = shift @ARGV if @ARGV && defined $ARGV[0] && length $ARGV[0];
    $lib   = shift @ARGV if @ARGV && defined $ARGV[0] && length $ARGV[0];
}

unshift @INC, $lib if defined $lib && length $lib;
my $dir = $ENV{TMPDIR} || '/tmp';

setup($dir, image_mb => $img_mb, json_mode => $json_mode);
MT::Image::freeze();
my $base_rss = rss_kb($$);

my $t0 = time;
my $pool = MT::Pool->new(workers => $n);
my $first = $pool->run(\&job, 0)->join;
my $startup = time - $t0;

$pool->map(\&job, 1 .. 2 * $n);
my $rss = rss_kb($$);
my $rss_per_w = rss_per_worker_kb($rss, $base_rss, $n);

my $start = time;
my $done = 0;
if ($fibers > 1) {
    my $chunk = int(($items + $n - 1) / $n);
    my @specs;
    for my $w (0 .. $n - 1) {
        my $lo = 1 + $w * $chunk;
        my $cnt = $chunk;
        $cnt = $items - $lo + 1 if $lo + $cnt - 1 > $items;
        next if $cnt <= 0;
        push @specs, "$lo:$cnt:$fibers";
    }
    my @sums = $pool->map(\&fiber_batch, @specs);
    $done = $items;
}
else {
    my @sums = $pool->map(\&job, 1 .. $items);
    $done = scalar @sums;
}
my $elapsed = time - $start;
$pool->shutdown;

my $drv = $fibers > 1 ? "mt-fibers$fibers" : "mt";
printf "driver=%s json_mode=%s image_mb=%d workers=%d fibers=%d items=%d ops_per_s=%.1f rss_kb=%d base_rss_kb=%d rss_per_worker_kb=%d startup_ms=%.2f\n",
    $drv, json_mode_name(), image_mb(), $n, $fibers, $items,
    $done / $elapsed, $rss, $base_rss, $rss_per_w, 1000 * $startup;

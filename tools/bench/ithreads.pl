#!/usr/bin/perl
# perl5-mt: reworked ithreads baseline driver.
#
# Spawns N ithreads after loading the same preloaded image (PERL_BENCH_IMAGE_MB),
# reporting ops_per_s, rss_kb, base_rss_kb, rss_per_worker_kb, and startup_ms.
#
#   perl ithreads.pl [workers] [items] [libdir]

use strict;
use warnings;
use threads;
use Thread::Queue;
use Time::HiRes qw(time);
use FindBin;
use lib "$FindBin::Bin";
use Workload qw(setup item rss_kb rss_per_worker_kb json_mode_name image_mb);

my $n      = 4;
my $items  = 2000;
my $img_mb = $ENV{PERL_BENCH_IMAGE_MB} // 0;
my $lib;

if (@ARGV && $ARGV[0] =~ /^--/) {
    while (@ARGV) {
        my $opt = shift @ARGV;
        if    ($opt eq '--workers')  { $n      = shift @ARGV }
        elsif ($opt eq '--items')    { $items  = shift @ARGV }
        elsif ($opt eq '--image-mb') { $img_mb = shift @ARGV }
        elsif ($opt eq '--lib')      { $lib    = shift @ARGV }
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

setup($dir, image_mb => $img_mb);
my $base_rss = rss_kb($$);

my $jobs = Thread::Queue->new;
my $done_q = Thread::Queue->new;
my $t0 = time;
my @thr = map {
    threads->create(sub {
        my $first = 1;
        while (defined(my $i = $jobs->dequeue)) {
            my $sum = item($i);
            if ($first) { $done_q->enqueue("start " . (time - $t0)); $first = 0 }
            $done_q->enqueue("ok $sum");
        }
        return 1;
    });
} 1 .. $n;

for my $i (1 .. 2 * $n) { $jobs->enqueue($i) }
my $seen = 0;
my $startup;
while ($seen < 2 * $n) {
    my $m = $done_q->dequeue;
    if ($m =~ /^start ([\d.]+)/) { $startup = $1 if !defined $startup || $1 < $startup; next }
    $seen++;
}
my $rss = rss_kb($$);
my $rss_per_w = rss_per_worker_kb($rss, $base_rss, $n);

my $start = time;
$jobs->enqueue($_) for 1 .. $items;
$jobs->end;
my $done = 0;
while ($done < $items) {
    my $m = $done_q->dequeue;
    if ($m =~ /^start ([\d.]+)/) { $startup = $1 if !defined $startup || $1 < $startup; next }
    $done++;
}
my $elapsed = time - $start;
$_->join for @thr;
printf "driver=ithreads json_mode=%s image_mb=%d workers=%d fibers=1 items=%d ops_per_s=%.1f rss_kb=%d base_rss_kb=%d rss_per_worker_kb=%d startup_ms=%.2f\n",
    json_mode_name(), image_mb(), $n, $items,
    $done / $elapsed, $rss, $base_rss, $rss_per_w, 1000 * ($startup // 0);

#!/usr/bin/perl
# perl5-mt: reworked prefork baseline driver.
#
# Forks N workers after loading the same preloaded image (PERL_BENCH_IMAGE_MB),
# reporting ops_per_s, total RSS across parent+children, base_rss_kb,
# rss_per_worker_kb, and startup_ms.
#
#   perl prefork.pl [workers] [items] [libdir]

use strict;
use warnings;
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

pipe(my $jr, my $jw) or die "pipe: $!";
pipe(my $rr, my $rw) or die "pipe: $!";
for my $fh ($jw, $rw) { my $old = select $fh; $| = 1; select $old }
my $t0 = time;
my @kids;
for my $k (1 .. $n) {
    my $pid = fork;
    die "fork: $!" unless defined $pid;
    if ($pid == 0) {
        close $jw; close $rr;
        my $first = 1;
        while (defined(my $line = <$jr>)) {
            chomp $line;
            my $sum = item($line);
            if ($first) { print $rw "start ", time - $t0, "\n"; $first = 0 }
            print $rw "ok $sum\n";
        }
        exit 0;
    }
    push @kids, $pid;
}
close $jr; close $rw;

for my $i (1 .. 2 * $n) { print $jw "$i\n" }
my $seen = 0;
my $startup;
while ($seen < 2 * $n) {
    my $l = <$rr>;
    if ($l =~ /^start ([\d.]+)/) { $startup = $1 if !defined $startup || $1 < $startup; next }
    $seen++;
}
my $rss = rss_kb($$, @kids);
my $rss_per_w = rss_per_worker_kb($rss, $base_rss, $n);

my $start = time;
for my $i (1 .. $items) { print $jw "$i\n" }
close $jw;
my $done = 0;
while (my $l = <$rr>) {
    if ($l =~ /^start ([\d.]+)/) { $startup = $1 if !defined $startup || $1 < $startup; next }
    $done++;
}
my $elapsed = time - $start;
waitpid($_, 0) for @kids;
printf "driver=prefork json_mode=%s image_mb=%d workers=%d fibers=1 items=%d ops_per_s=%.1f rss_kb=%d base_rss_kb=%d rss_per_worker_kb=%d startup_ms=%.2f\n",
    json_mode_name(), image_mb(), $n, $items,
    $done / $elapsed, $rss, $base_rss, $rss_per_w, 1000 * ($startup // 0);

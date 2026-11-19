#!./perl
# perl5-mt: the round-trip latency of a trivial remote XS call (a legacy
# XSUB served by an XS compartment), as reported in MT_README.md.
#
#   ./perl -Ilib tools/mt-xs-latency.pl [iterations] [mode]   (default 1_000_000 thread)
#
# modes:
#   thread      a plain worker, the default adaptive wait
#   parked      a plain worker with PERL_KRT_POLL_NS=0: the waiter parks at
#               once, which is what the bridge paid before the vectors
#   fibers:N    N scheduled fibers of one worker call in flight at once; the
#               latency is the per-call time a fiber sees (queueing included),
#               the rate is the calls of all fibers per second
#   smt         a plain worker with PERL_MT_XS_AFFINITY=smt (Linux pins the
#               worker to an SMT sibling of its compartment; elsewhere no-op)
#
# Each mode runs five rounds of `iterations` calls of MT::XSTest::Legacy::sum(1, 2)
# inside one worker and prints the median per-call time and the median
# rate, one line.  The in-process tier (MT::XSTest::add) is printed for
# comparison in thread mode.

use strict;
use warnings;
use Time::HiRes qw(time);
use Config;

my $n = $ARGV[0] || 1_000_000;
my $mode = $ARGV[1] || 'thread';
my $nf = 0;
if ($mode =~ /^fibers:(\d+)$/) {
    $nf = $1;
} elsif ($mode eq 'parked') {
    $ENV{PERL_KRT_POLL_NS} = '0';
} elsif ($mode eq 'smt') {
    $ENV{PERL_MT_XS_AFFINITY} = 'smt';
} elsif ($mode ne 'thread') {
    die "unknown mode '$mode' (thread, parked, fibers:N, smt)\n";
}

sub median {
    my @s = sort { $a <=> $b } @_;
    return $s[int(@s / 2)];
}

# thread modes: the time of one call, median of five rounds
sub measure {
    my ($code) = @_;
    my @t;
    for my $rep (1 .. 5) {
        my $t = time;
        $code->() for 1 .. $n;
        push @t, (time - $t) / $n * 1e6;
    }
    return median(@t);
}

# fiber modes: nf fibers, each makes per calls; one round returns the
# median per-call time over the fibers and the calls per second of all
sub fiber_round {
    my ($per) = @_;
    my @ft;
    my $t0 = time;
    my @ids = map {
        MT::Fiber::schedule(sub {
            my $t = time;
            MT::XSTest::Legacy::sum(1, 2) for 1 .. $per;
            push @ft, (time - $t) / $per * 1e6;
            return 1;
        });
    } 1 .. $nf;
    MT::Fiber::run();
    MT::Fiber::join($_) for @ids;
    my $wall = time - $t0;
    return (median(@ft), $nf * $per / $wall);
}

sub work_thread {
    require MT::XSTest::Legacy;
    require MT::XSTest;
    my $rmed = measure(sub { MT::XSTest::Legacy::sum(1, 2) });
    my $lmed = measure(sub { MT::XSTest::add(1, 2) });
    my $bs = MT::XS::bridge_stats();
    return sprintf "%.2f %.0f %.3f %s %d %d",
        $rmed, 1e6 / $rmed, $lmed, $bs->{wait_tier} // '?', $bs->{polled} // 0, $bs->{parked} // 0;
}

sub work_fibers {
    require MT::XSTest::Legacy;
    my $per = int($n / $nf) || 1;
    my (@med, @rate);
    fiber_round(int($per / 10) || 1);            # warm up
    for (1 .. 5) {
        my ($m, $r) = fiber_round($per);
        push @med, $m;
        push @rate, $r;
    }
    return sprintf "%.2f %.0f 0 %s 0 0", median(@med), median(@rate),
        MT::XS::bridge_stats()->{wait_tier} // '?';
}

MT::Image::freeze();
my $sub = $nf ? 'main::work_fibers' : 'main::work_thread';
my $r = MT::Worker::join(MT::Worker::spawn($sub));
my ($med, $rate, $lmed, $tier, $polled, $parked) = split ' ', $r;
my $bs_pin = '';
printf "%s %s, %s, %d calls x 5 rounds: median %.2f us per call, %.0f calls/s"
     . " (wait tier %s; in-process %s us; polled %s, parked %s)\n",
    $Config{archname}, $^O, $mode, $n, $med, $rate, $tier,
    $lmed, $polled, $parked;

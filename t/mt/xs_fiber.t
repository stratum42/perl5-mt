#!./perl
# perl5-mt: a scheduled fiber parks on a remote call instead of
# blocking its worker, and the bridge's replies are drained by the scheduler
# (the idle hook and the poll of each round).  The fixture is
# ext/MT-XSTest-Legacy, served by an XS compartment.  Every case runs in a
# child perl: a frozen image and its compartments are process-wide.

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib');
}

use strict;
use warnings;
use Config;

plan(tests => 10);

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

my $prelude = <<'PRELUDE';
$| = 1;
sub say_ { print @_, "\n" }
sub run_worker { my ($sub, @args) = @_; return MT::Worker::join(MT::Worker::spawn($sub, @args)) }
sub err { my ($e) = @_; $e =~ s/ at \S+ line \d+\.?\n?//; $e =~ s/\n+$//; return $e }
sub tally { my %s; $s{$_}++ for @_; return join " ", map { "$_=$s{$_}" } sort keys %s }
PRELUDE

# the fixture must load, or every case is skipped (as in xs_bridge.t)
{
    my $out = child(q{require MT::XSTest::Legacy; print "ok\n"});
    skip_all("MT::XSTest::Legacy is not built (make -C ext/MT-XSTest-Legacy)")
        unless $out =~ /^ok$/m;
}

# ---- (a) 64 fibers, 1000 remote calls each, every result checked ----------

my $case_a = <<'CODE';
sub work {
    require MT::XSTest::Legacy;
    my @ids = map {
        my $k = $_;
        MT::Fiber::schedule(sub {
            my $bad = 0;
            for my $i (1 .. 1000) {
                my $r = MT::XSTest::Legacy::sum($k, $i);
                $bad++ unless $r == $k + $i;
            }
            return $bad;
        });
    } 1 .. 64;
    MT::Fiber::run();
    my $bad = 0;
    $bad += MT::Fiber::join($_) for @ids;
    return "bad=$bad";
}
MT::Image::freeze();
say_ "a: ", run_worker('main::work');
CODE

{
    my $out = child($prelude . $case_a);
    like($out, qr/^a: bad=0$/m,
         '64 scheduled fibers in one worker each make 1000 remote calls, every result right');
}

# ---- (b) a napping call never stalls the other fibers ---------------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my ($passes, $napped, $done) = (0, -1, 0);
    MT::Fiber::schedule(sub {
        MT::XSTest::Legacy::nap_ms(200);
        $napped = $passes;
        $done = 1;
        return 1;
    });
    for (1 .. 8) {
        MT::Fiber::schedule(sub {
            while (!$done && $passes < 50_000_000) { $passes++; MT::Fiber::pass() }
            return 1;
        });
    }
    MT::Fiber::run();
    return $napped >= 1000 ? "passes=ok" : "passes=$napped";
}
MT::Image::freeze();
say_ "b: ", run_worker('main::work');
CODE
    like($out, qr/^b: passes=ok$/m,
         'a fiber in a 200 ms call: eight other fibers pass at least 1000 times meanwhile');
}

# ---- (c) a compartment callback during a fiber's call -----------------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my $host_runs = 0;
    my @ids = map {
        my $k = $_;
        MT::Fiber::schedule(sub {
            my ($r) = MT::XSTest::Legacy::callback(sub { $host_runs++; return $_[0] * 2 }, $k);
            return $r == 2 * $k ? "right" : "wrong:" . (defined $r ? $r : 'undef');
        });
    } 1 .. 16;
    MT::Fiber::run();
    my @r = map { MT::Fiber::join($_) } @ids;
    return tally(@r) . " host_runs=$host_runs";
}
MT::Image::freeze();
say_ "c: ", run_worker('main::work');
CODE
    like($out, qr/^c: right=16 host_runs=16$/m,
         'a compartment callback during a fiber call runs and the fiber gets its own result');
}

# ---- (d) a croak in the compartment is rethrown in the fiber that called --

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my @ids = map {
        my $k = $_;
        MT::Fiber::schedule(sub {
            my $e = eval { MT::XSTest::Legacy::croak_with("boom$k"); 1 } ? "none" : err($@);
            return $e;
        });
    } 1 .. 8;
    MT::Fiber::run();
    my @r = map { MT::Fiber::join($_) } @ids;
    my $ok = 0;
    for my $k (1 .. 8) { $ok++ if $r[$k - 1] =~ /boom$k\b/ }
    return "rethrown=$ok";
}
MT::Image::freeze();
say_ "d: ", run_worker('main::work');
CODE
    like($out, qr/^d: rethrown=8$/m,
         'a croak in the compartment is rethrown in the fiber that made the call, not another');
}

# ---- (e) too many calls in flight: the 65th croaks ------------------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my @ids = map {
        MT::Fiber::schedule(sub {
            eval { MT::XSTest::Legacy::nap_ms(100); 1 } ? "ok" : err($@);
        });
    } 1 .. 65;
    MT::Fiber::run();
    my @r = map { MT::Fiber::join($_) } @ids;
    my %n;
    for (@r) { $n{ /MT_XS_TOO_MANY_INFLIGHT/ ? 'toomany' : $_ }++ }
    return join " ", map { "$_=$n{$_}" } sort keys %n;
}
MT::Image::freeze();
say_ "e: ", run_worker('main::work');
CODE
    like($out, qr/^e: ok=64 toomany=1$/m,
         '65 fibers calling at once: the 65th gets MT_XS_TOO_MANY_INFLIGHT, the rest run');
}

# ---- (f) a compartment crashed under four parked fibers -------------------

{
    my $out = child($prelude . <<'CODE');
use Time::HiRes ();
sub work {
    require MT::XSTest::Legacy;
    MT::XSTest::Legacy::sum(0);                 # binds the worker
    my $comp = MT::XS::stats()->{compartment};
    my $crashed_at = 0;
    my @ids;
    # the napper holds the compartment for 300 ms, the crash queues behind
    # it and the four calls queue behind the crash: they can only ever be
    # answered by the death, which the reaper announces to their slot
    push @ids, MT::Fiber::schedule(sub {
        eval { MT::XSTest::Legacy::nap_ms(300); 1 } ? "napped" : err($@);
    });
    push @ids, MT::Fiber::schedule(sub {
        MT::XS::_crash($comp);
        $crashed_at = Time::HiRes::time();
        return "crashed";
    });
    push @ids, map {
        MT::Fiber::schedule(sub {
            my $e = eval { MT::XSTest::Legacy::sum(1, 2); 1 } ? "alive" : err($@);
            return ($e =~ /MT_COMPARTMENT_DEAD/ ? "dead" : "other:$e") . "|" . Time::HiRes::time();
        });
    } 1 .. 4;
    MT::Fiber::run();
    my @r = map { MT::Fiber::join($_) } @ids[2 .. 5];
    my $dead = grep { /^dead\|/ } @r;
    my ($latest) = sort { $b <=> $a } map { (split /\|/, $_)[1] } @r;
    my $fast = $crashed_at && $latest - $crashed_at < 1.0 ? "fast" : "slow";
    return "dead=$dead,$fast";
}
MT::Image::freeze();
say_ "f: ", run_worker('main::work');
CODE
    like($out, qr/^f: dead=4,fast$/m,
         'a compartment crashed under four parked fibers: all four die with MT_COMPARTMENT_DEAD within a second');
}

# ---- (g) thread mode: a worker with no scheduler loop -------------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my $bad = 0;
    for my $i (1 .. 10000) {
        $bad++ unless MT::XSTest::Legacy::sum($i, 1) == $i + 1;
    }
    return "thread=$bad";
}
MT::Image::freeze();
say_ "g: ", run_worker('main::work');
CODE
    like($out, qr/^g: thread=0$/m,
         'a worker with no scheduler loop makes 10000 remote calls, every result right');
}

# ---- (h) sixteen workers, sixteen fibers each -------------------------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    my ($w) = @_;
    require MT::XSTest::Legacy;
    MT::XSTest::Legacy::sum(0);
    my $comp = MT::XS::stats()->{compartment};
    my @ids = map {
        my $f = $_;
        MT::Fiber::schedule(sub {
            my $bad = 0;
            for my $i (1 .. 100) {
                $bad++ unless MT::XSTest::Legacy::sum($w, $f, $i) == $w + $f + $i;
            }
            return $bad;
        });
    } 1 .. 16;
    MT::Fiber::run();
    my $bad = 0;
    $bad += MT::Fiber::join($_) for @ids;
    my $stable = MT::XS::stats()->{compartment} == $comp;
    return ($bad == 0 && $stable) ? "ok" : "bad=$bad,stable=" . ($stable ? 1 : 0);
}
MT::Image::freeze();
my @ids = map { MT::Worker::spawn('main::work', $_) } 1 .. 16;
say_ "h: ", tally(map { MT::Worker::join($_) } @ids);
CODE
    like($out, qr/^h: ok=16$/m,
         'sixteen workers each run sixteen fibers of remote calls at once; every result right, handles stay');
}

# ---- (i) PERL_MT_XS_AFFINITY=smt: the same fan-out, and pinning ---------------

SKIP: {
    skip("affinity pinning is a Linux feature", 2) if $^O eq 'darwin';
    local $ENV{PERL_MT_XS_AFFINITY} = 'smt';
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my @ids = map {
        my $k = $_;
        MT::Fiber::schedule(sub {
            my $bad = 0;
            for my $i (1 .. 100) { $bad++ unless MT::XSTest::Legacy::sum($k, $i) == $k + $i }
            return $bad;
        });
    } 1 .. 16;
    MT::Fiber::run();
    my $bad = 0;
    $bad += MT::Fiber::join($_) for @ids;
    my $pinned = MT::XS::bridge_stats()->{pinned_cpu};
    return "bad=$bad,affinity=" . MT::XS::bridge_stats()->{affinity} . ",pinned=" . ($pinned >= 0 ? 1 : 0);
}
MT::Image::freeze();
my @ids = map { MT::Worker::spawn('main::work', $_) } 1 .. 4;
say_ "i: ", join " ", map { MT::Worker::join($_) } @ids;
CODE
    like($out, qr/^i: (?:bad=0,affinity=smt,pinned=[01]\s*)+$/m,
         'PERL_MT_XS_AFFINITY=smt: fibers still right, and the bridge reports the pinning');
    like($out, qr/pinned=1/m, 'PERL_MT_XS_AFFINITY=smt pins at least one worker to an SMT sibling');
}

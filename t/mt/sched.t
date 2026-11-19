#!./perl
# perl5-mt: the fiber scheduler (mt_fiber.c).  Scheduled fibers
# run in a ready queue under one loop per interpreter; park and wake, the
# tick at PERL_ASYNC_CHECK that preemption uses, the deadlock report, and
# fibers in workers.  Every case runs in a child perl, as fiber.t does: a
# frozen image is process-wide.

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib');
}

use strict;
use warnings;

plan(tests => 21);

my $long = $ENV{PERL_MT_LONG_TESTS};

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

# The batch routine of (h) and (i): n fibers, a hundred at a time, each
# passing a hundred times and returning its number.  Its sum is n(n+1)/2.
my $batch = <<'CODE';
sub batch {
    my ($n) = @_;
    my $sum = 0;
    for (my $base = 0; $base < $n; $base += 100) {
        my @ids = map {
            my $v = $base + $_;
            MT::Fiber::schedule(sub { for (1 .. 100) { MT::Fiber::pass() } $v });
        } 1 .. 100;
        MT::Fiber::run();
        $sum += MT::Fiber::join($_) for @ids;
    }
    return $sum;
}
CODE

# ---- (a) three fibers, exact round robin -----------------------------------

{
    my $out = child(<<'CODE');
my @log;
my @f = map {
    my $n = $_;
    MT::Fiber::schedule(sub { for (1 .. 10) { push @log, $n; MT::Fiber::pass() } });
} 1 .. 3;
MT::Fiber::run();
print "@log\n";
CODE
    my $want = join(' ', (1, 2, 3) x 10);
    is($out, "$want\n", 'three scheduled fibers that pass ten times run in exact round robin');
}

# ---- (b) park and wake, a thousand times ----------------------------------

{
    my $out = child(<<'CODE');
my ($slot, $cid);
my $sum = 0;
$cid = MT::Fiber::schedule(sub {
    for (1 .. 1000) { MT::Fiber::park(); $sum += $slot }
    return $sum;
});
MT::Fiber::schedule(sub {
    for my $i (1 .. 1000) { $slot = $i; MT::Fiber::wake($cid); MT::Fiber::pass() }
});
MT::Fiber::run();
print MT::Fiber::join($cid), " ", MT::Fiber::count(), "\n";
CODE
    is($out, "500500 0\n", 'a consumer parked and woken by a producer 1000 times, every value seen');
}

# ---- (c) run counts the finished fibers; join returns every result ---------

{
    my $out = child(<<'CODE');
my @f = map {
    my $n = $_;
    MT::Fiber::schedule(sub { MT::Fiber::pass(); $n * 10 });
} 1 .. 4;
my $d = MT::Fiber::schedule(sub { MT::Fiber::pass(); die bless({ code => 42 }, 'Err') });
my $n = MT::Fiber::run();
print "n=$n\n";
print join(",", map { MT::Fiber::join($_) } @f), "\n";
eval { MT::Fiber::join($d) };
print ref($@), " ", $@->{code}, "\n";
eval { MT::Fiber::join($d) };
print "again: $@";
CODE
    like($out, qr/^n=5$/m, 'run returns the number of fibers that finished');
    like($out, qr/^10,20,30,40$/m, 'join returns the result of every scheduled fiber');
    like($out, qr/^Err 42$/m, 'a died scheduled fiber is rethrown by join with its object');
}

# ---- (d) a deadlock names the parked count ---------------------------------

{
    my $out = child(<<'CODE');
MT::Fiber::schedule(sub { MT::Fiber::park() });
MT::Fiber::schedule(sub { MT::Fiber::park() });
eval { MT::Fiber::run() };
print "err: $@";
CODE
    like($out, qr/^err: perl5-mt: scheduler deadlock: 2 parked, none ready \(MT_FIBER_DEADLOCK\)/m,
         'parked fibers with nothing to wake them are MT_FIBER_DEADLOCK, with the count');
}

# ---- (e) a nested run is refused -------------------------------------------

{
    my $out = child(<<'CODE');
my $err;
MT::Fiber::schedule(sub { eval { MT::Fiber::run() }; $err = $@ });
MT::Fiber::run();
print "nested: $err";
CODE
    like($out, qr/^nested: perl5-mt: a scheduler loop is already running .*\(MT_SCHED_NESTED\)/m,
         'run inside a scheduled fiber is MT_SCHED_NESTED');
}

# ---- (f) preemption: a woken fiber runs while a tight loop spins -----------

{
    my $body = <<'CODE';
MT::Fiber::preempt(PREEMPT);
my ($bprog, $seen) = (0, 'unset');
my $a = MT::Fiber::schedule(sub {
    my $x = 0;
    for (1 .. 2_000_000) { $x++ }
    $seen = $bprog;
    return $x;
});
my $b = MT::Fiber::schedule(sub { $bprog++; 'b' });
MT::Fiber::wake($b);
MT::Fiber::run();
print "seen=$seen ticks=", MT::Fiber::sched_stats()->{ticks}, "\n";
CODE
    (my $on = $body) =~ s/PREEMPT/1/;
    (my $off = $body) =~ s/PREEMPT/0/;
    my $out = child($on);
    like($out, qr/^seen=1 ticks=1$/m,
         'with preemption, the wake of B is a tick and B runs before A finishes');
    $out = child($off);
    like($out, qr/^seen=0 ticks=0$/m,
         'without preemption, B waits for A to finish, and no tick is taken');
}

# ---- (g) a wake during compilation does not switch -------------------------

{
    my $out = child(<<'CODE');
$main::ran = 0;
# the waker is scheduled first, so that it runs before the woken fiber
MT::Fiber::schedule(sub {
    my $r = eval q{
        BEGIN {
            MT::Fiber::wake($main::bid);
            for (1 .. 100_000) { }
            $main::during = $main::ran;
        }
        'compiled'
    };
    print "eval=", (defined $r ? $r : "undef: $@"), " during=$main::during\n";
    return 'a';
});
$main::bid = MT::Fiber::schedule(sub { $main::ran++; 'b' });
MT::Fiber::preempt(1);
MT::Fiber::run();
print "ran=$main::ran\n";
CODE
    like($out, qr/^eval=compiled during=0$/m,
         'a wake inside BEGIN does not switch fibers, and the eval completes');
    like($out, qr/^ran=1$/m, '... and the woken fiber runs once the loop comes back to it');
}

# ---- (h) ten thousand fibers, a hundred passes each, stacklets reused ------

{
    my $n = $long ? 100_000 : 10_000;
    my $code = $batch . <<'CODE';
my $sum = batch(NN);
my $s = MT::Fiber::stats();
print "sum=$sum mmaps=$s->{mmaps} peak=$s->{peak} count=", MT::Fiber::count(), "\n";
CODE
    $code =~ s/\bNN\b/$n/;
    my $out = child($code);
    my $want = $n * ($n + 1) / 2;
    like($out, qr/^sum=\Q$want\E mmaps=100 peak=100 count=0$/m,
         "$n fibers, a hundred at a time, each passing a hundred times");
}

# ---- (i) the same in a worker, and in sixteen workers at once --------------

{
    my $n = $long ? 100_000 : 10_000;
    my $want = $n * ($n + 1) / 2;
    my $code = $batch . <<'CODE';
sub f {
    my $sum = batch(NN);
    my $s = MT::Fiber::stats();
    return "sum=$sum mmaps=$s->{mmaps} peak=$s->{peak}";
}
CODE
    $code =~ s/\bNN\b/$n/;
    my $out = child($code . <<'CODE');
MT::Image::freeze();
print MT::Worker::join(MT::Worker::spawn('main::f')), "\n";
CODE
    is($out, "sum=$want mmaps=100 peak=100\n", 'the batch in a worker');
    $out = child($code . <<'CODE');
MT::Image::freeze();
my @w = map { MT::Worker::spawn('main::f') } 1 .. 16;
my %r;
$r{ MT::Worker::join($_) }++ for @w;
print join(",", map { "$_ x$r{$_}" } sort keys %r), "\n";
CODE
    is($out, "sum=$want mmaps=100 peak=100 x16\n", 'the batch in sixteen workers at once');
}

# ---- (j) the calls that need a scheduled fiber, and that refuse one --------

{
    my $out = child(<<'CODE');
my $f = MT::Fiber::spawn(sub { eval { MT::Fiber::park(); 1 } ? 'parked' : $@ });
print "park: ", MT::Fiber::resume($f), "\n";
my $g = MT::Fiber::spawn(sub { eval { MT::Fiber::pass(); 1 } ? 'passed' : $@ });
print "pass: ", MT::Fiber::resume($g), "\n";
my $s = MT::Fiber::schedule(sub { 'x' });
eval { MT::Fiber::resume($s) };
print "resume: $@";
eval { MT::Fiber::join($s) };
print "join: $@";
MT::Fiber::run();
print "joined: ", MT::Fiber::join($s), "\n";
CODE
    like($out, qr/^park: .*\(MT_FIBER_NOT_SCHEDULED\)/m,
         'park in an explicitly resumed fiber is MT_FIBER_NOT_SCHEDULED');
    like($out, qr/^pass: .*\(MT_FIBER_NOT_SCHEDULED\)/m,
         'pass in an explicitly resumed fiber is MT_FIBER_NOT_SCHEDULED');
    like($out, qr/^resume: .*\(MT_FIBER_IS_SCHEDULED\)/m,
         'resume of a scheduled fiber is MT_FIBER_IS_SCHEDULED');
    like($out, qr/^join: .*\(MT_FIBER_NOT_FINISHED\)/m,
         'join of an unfinished scheduled fiber is MT_FIBER_NOT_FINISHED, not a resume');
    like($out, qr/^joined: x$/m, '... and the loop still finishes it');
}

# ---- (k) fibers scheduled from a scheduled fiber join the same queue -------

{
    my $out = child(<<'CODE');
my @log;
MT::Fiber::schedule(sub {
    push @log, 'parent';
    for my $n (1 .. 3) {
        MT::Fiber::schedule(sub { MT::Fiber::pass(); push @log, "kid$n" });
    }
    MT::Fiber::pass();
    push @log, 'parent-after';
});
my $n = MT::Fiber::run();
print "n=$n log=@log\n";
CODE
    is($out, "n=4 log=parent parent-after kid1 kid2 kid3\n",
       'fibers scheduled inside a scheduled fiber run in the same loop');
}

# ---- the cost of one scheduled pass, for the record ------------------------

{
    my $n = $long ? 1_000_000 : 100_000;
    my $code = <<'CODE';
require Time::HiRes;
my $n = NN;
MT::Fiber::schedule(sub { MT::Fiber::pass() for 1 .. $n; 1 });
my $t0 = Time::HiRes::time();
MT::Fiber::run();
my $dt = Time::HiRes::time() - $t0;
printf "%.1f\n", $dt / $n * 1e9;
CODE
    $code =~ s/\bNN\b/$n/;
    my $out = child($code);
    my ($ns) = $out =~ /^(\d+\.\d)$/m;
    ok(defined $ns, 'scheduled pass round trips ran');
    note("scheduled pass round trip: $ns ns ($n passes, one fiber, loop resumes and passes)")
        if defined $ns;
}

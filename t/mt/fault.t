#!./perl
# perl5-mt: fault-injection test suite covering:
#   - 6c: compartment crash (MT::XS::_crash & kill -9), pre-forked spare
#         restart, module reload, and MT_STALE_REMOTE_HANDLE on stale handles
#   - 6d: per-worker signal containment (SIGSEGV, SIGBUS, SIGFPE) and OOM
#   - 6b: heartbeat timeout detection within deadline
#   - 6e: MT::Pool supervision (respawn, exponential backoff, restart cap,
#         ->faults, exact item accounting under worker kills) and named
#         endpoints (MT::Pool->named, MT::Channel->named)
#   - 6f: explicit checkpoints (in-memory & file-backed) and process-pair
#         backup takeover (MT::Pool->new(pairs => 1))

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib', '../ext/MT/lib');
}

use strict;
use warnings;

my $long = $ENV{PERL_MT_LONG_TESTS};
plan(tests => 19);

sub child {
    my ($code, %opt) = @_;
    $code = "BEGIN { unshift \@INC, '../ext/MT/lib' }\n" . $code;
    my $file = tempfile();
    open my $fh, ">", $file or die "open $file: $!";
    print $fh $code;
    close $fh or die "close $file: $!";
    my $out = runperl(progfile => $file, stderr => 1, %opt);
    $out =~ s/\r\n/\n/g;
    return $out;
}

# ---- 1. compartment crash (_crash & kill -9) + spare restart -----
{
    local $ENV{PERL_MT_XS_COMPARTMENTS} = 1;
    local $ENV{PERL_MT_XS_SPARES}       = 2;
    my $out = child(<<'PERL');
use strict; use warnings;
use Time::HiRes qw(time sleep);
use MT::Fault;

MT::Image::freeze();
MT::Fault::reset();

my $st0 = MT::XS::stats();
my ($c0) = MT::XS::compartments();
MT::XS::ping(0);
my $pid1 = $c0->{pid};
my $gen1 = $c0->{gen};
print "init: pid=$pid1 gen=$gen1 spares=$st0->{spares_available}/$st0->{spares_total}\n";

# Crash via MT::XS::_crash(0)
my $t0 = time();
MT::XS::_crash(0);
my $c1;
while (time() - $t0 < 5.0) {
    ($c1) = MT::XS::compartments();
    last if $c1->{gen} >= 2 && $c1->{alive} == 1 && $c1->{restarts} >= 1;
    sleep(0.005);
}
my $dt1_ms = sprintf("%.2f", (time() - $t0) * 1000);
MT::XS::ping(0);
my $st1 = MT::XS::stats();
my $f1 = MT::Fault::last();
print "after_crash: alive=$c1->{alive} gen=$c1->{gen} pid_changed=",
      ($c1->{pid} != $pid1 ? 1 : 0), " restarts=$c1->{restarts}",
      " spares=$st1->{spares_available} fault=$f1->{cause}:$f1->{unit_kind}",
      " dt_ms=$dt1_ms\n";

# Kill -9 the restarted compartment directly
my $pid2 = $c1->{pid};
my $t1 = time();
kill 'KILL', $pid2;
my $c2;
while (time() - $t1 < 5.0) {
    ($c2) = MT::XS::compartments();
    last if $c2->{gen} >= 3 && $c2->{alive} == 1 && $c2->{restarts} >= 2;
    sleep(0.005);
}
my $dt2_ms = sprintf("%.2f", (time() - $t1) * 1000);
MT::XS::ping(0);
my $st2 = MT::XS::stats();
print "after_kill9: alive=$c2->{alive} gen=$c2->{gen} pid_changed=",
      ($c2->{pid} != $pid2 ? 1 : 0), " restarts=$c2->{restarts}",
      " spares=$st2->{spares_available} dt_ms=$dt2_ms\n";
PERL
    like($out, qr/^init: pid=\d+ gen=1 spares=2\/2$/m,
         '6c: pool starts with 2 pre-forked spares');
    like($out, qr/^after_crash: alive=1 gen=2 pid_changed=1 restarts=1 spares=1 fault=COMPARTMENT_CRASH:compartment dt_ms=/m,
         '6c: MT::XS::_crash triggers spare promotion, gen bump, and fault record');
    like($out, qr/^after_kill9: alive=1 gen=3 pid_changed=1 restarts=2 spares=0 dt_ms=/m,
         '6c: kill -9 triggers second spare promotion and compartment stays live');
    note("compartment restart output:\n$out");
}

# ---- 2. module reload on restart + MT_STALE_REMOTE_HANDLE --------
{
    local $ENV{PERL_MT_XS_COMPARTMENTS} = 1;
    local $ENV{PERL_MT_XS_SPARES}       = 1;
    my $dir = tempfile();
    mkdir $dir or die "mkdir $dir: $!";
    open my $mfh, '>', "$dir/FaultBox.pm" or die "open FaultBox.pm: $!";
    print $mfh <<'PM';
package FaultBox;
use strict; use warnings;
sub new { bless { v => $_[1] }, $_[0] }
sub get { $_[0]->{v} }
sub ping_pid { $$ }
1;
PM
    close $mfh;

    my $out = child(<<"PERL");
use strict; use warnings;
use Time::HiRes qw(time sleep);
unshift \@INC, '$dir';

sub work {
    my \$obj = MT::XS::remote('FaultBox')->new(42);
    my \$v1  = \$obj->get();
    my \$comp = MT::XS::handle_compartment(\$obj);
    my \$p1  = MT::XS::remote('FaultBox')->ping_pid();

    MT::XS::_crash(\$comp);
    my \$t0 = time();
    while (time() - \$t0 < 5.0) {
        my (\$c) = grep { \$_->{id} == \$comp } MT::XS::compartments();
        last if \$c && \$c->{gen} >= 2 && \$c->{alive} == 1 && \$c->{restarts} >= 1;
        sleep(0.005);
    }

    eval { \$obj->get() };
    my \$stale_ok = (\$\@ =~ /MT_STALE_REMOTE_HANDLE/) ? 1 : 0;

    my \$obj2 = MT::XS::remote('FaultBox')->new(99);
    my \$v2   = \$obj2->get();
    my \$p2   = MT::XS::remote('FaultBox')->ping_pid();
    my (\$c)  = grep { \$_->{id} == \$comp } MT::XS::compartments();
    return "v1=\$v1 stale=\$stale_ok v2=\$v2 pid_diff=" . (\$p1 != \$p2 ? 1 : 0)
         . " loaded=\$c->{loaded_modules} gen=\$c->{gen}";
}

MT::Image::freeze();
my \$wid = MT::Worker::spawn('main::work');
print MT::Worker::join(\$wid), "\\n";
PERL
    unlink "$dir/FaultBox.pm";
    rmdir $dir;
    like($out, qr/^v1=42 stale=1 v2=99 pid_diff=1 loaded=[1-9]\d* gen=2$/m,
         '6c: stale handle croaks MT_STALE_REMOTE_HANDLE and reloaded module serves new calls');
}

# ---- 3. per-worker SIGSEGV, SIGBUS, SIGFPE & OOM containment -----
{
    my $out = child(<<'PERL');
use strict; use warnings;
use MT::Fault;

sub healthy {
    my ($k) = @_;
    my $s = 0;
    $s += $_ for 1 .. 1000;
    return $s + $k;
}
sub do_segv { MT::Fault::_trigger_sigsegv(); return "unreachable" }
sub do_bus  { MT::Fault::_trigger_sigbus();  return "unreachable" }
sub do_fpe  { MT::Fault::_trigger_sigfpe();  return "unreachable" }
sub do_oom  { MT::Fault::_trigger_oom();     return "unreachable" }

MT::Image::freeze();
MT::Fault::reset();

# Spawn 15 healthy workers + 1 SIGSEGV worker concurrently
my @good = map { MT::Worker::spawn('main::healthy', $_) } 1 .. 15;
my $bad_segv = MT::Worker::spawn('main::do_segv');

eval { MT::Worker::join($bad_segv) };
my $segv_err = $@;
my $f_segv = MT::Fault::last();

my $ok_siblings = 0;
for my $i (0 .. 14) {
    my $r = MT::Worker::join($good[$i]);
    $ok_siblings++ if $r == 500500 + ($i + 1);
}
print "segv: siblings=$ok_siblings faulted=",
      ($segv_err =~ /MT_WORKER_FAULTED.*SIGSEGV|SIGSEGV.*MT_WORKER_FAULTED/ ? 1 : 0),
      " cause=$f_segv->{cause} kind=$f_segv->{unit_kind}\n";

# SIGBUS
my $w_bus = MT::Worker::spawn('main::do_bus');
eval { MT::Worker::join($w_bus) };
my $bus_err = $@;
print "bus: faulted=", ($bus_err =~ /SIGBUS/ && $bus_err =~ /MT_WORKER_FAULTED/ ? 1 : 0), "\n";

# SIGFPE
my $w_fpe = MT::Worker::spawn('main::do_fpe');
eval { MT::Worker::join($w_fpe) };
my $fpe_err = $@;
print "fpe: faulted=", ($fpe_err =~ /SIGFPE/ && $fpe_err =~ /MT_WORKER_FAULTED/ ? 1 : 0), "\n";

# OOM
my $w_oom = MT::Worker::spawn('main::do_oom');
my $w_sib = MT::Worker::spawn('main::healthy', 7);
eval { MT::Worker::join($w_oom) };
my $oom_err = $@;
my $f_oom = MT::Fault::last();
my $sib_res = MT::Worker::join($w_sib);
print "oom: faulted=", ($oom_err =~ /OOM/ ? 1 : 0),
      " cause=$f_oom->{cause} sibling=", ($sib_res == 500507 ? 1 : 0), "\n";
PERL
    like($out, qr/^segv: siblings=15 faulted=1 cause=SIGNAL kind=worker$/m,
         '6d: SIGSEGV contained to faulting worker while 15 siblings finish intact');
    like($out, qr/^bus: faulted=1$/m,
         '6d: SIGBUS contained and reported as MT_WORKER_FAULTED');
    like($out, qr/^fpe: faulted=1$/m,
         '6d: SIGFPE contained and reported as MT_WORKER_FAULTED');
    like($out, qr/^oom: faulted=1 cause=OOM sibling=1$/m,
         '6d: OOM contained to worker without tearing down sibling');
}

# ---- 4. repeated SIGSEGV containment stress loop -----------------
{
    my $iters = $long ? 500 : 30;
    my $out = child(<<"PERL");
use strict; use warnings;
use MT::Fault;
sub crash_me { MT::Fault::_trigger_sigsegv() }
MT::Image::freeze();
MT::Fault::reset();
my \$caught = 0;
for (1 .. $iters) {
    my \$id = MT::Worker::spawn('main::crash_me');
    eval { MT::Worker::join(\$id) };
    \$caught++ if \$\@ =~ /MT_WORKER_FAULTED/ && \$\@ =~ /SIGSEGV/;
}
print "stress: caught=\$caught expected=$iters count=", MT::Fault::count(), "\\n";
PERL
    like($out, qr/^stress: caught=$iters expected=$iters count=$iters$/m,
         "6d: repeated worker SIGSEGV containment succeeds $iters/$iters times");
}

# ---- 5. heartbeat timeout detection ------------------------------
{
    local $ENV{PERL_MT_HEARTBEAT_MS}   = 25;
    local $ENV{PERL_MT_HEARTBEAT_MISS} = 3;
    my $out = child(<<'PERL');
use strict; use warnings;
use Time::HiRes qw(time sleep);
use MT::Fault;
use MT::Supervisor;
use MT::Channel;

sub hung_worker {
    my ($in_id, $out_id) = @_;
    my $in  = MT::Channel->attach($in_id);
    my $out = MT::Channel->attach($out_id);
    MT::Supervisor::beat();
    MT::Supervisor::_pause_heartbeat(1);
    $out->send("paused");
    $in->recv();
    return 1;
}

MT::Image::freeze();
MT::Fault::reset();
my $in  = MT::Channel->new(2);
my $out = MT::Channel->new(2);
my $wid = MT::Worker::spawn('main::hung_worker', $in->id, $out->id);
$out->recv();

my $t0 = time();
my $found = 0;
while (time() - $t0 < 2.0) {
    if (MT::Supervisor::tick() > 0) { $found = 1; last }
    sleep(0.01);
}
my $ms = int((time() - $t0) * 1000);
$in->send("stop");
MT::Worker::join($wid);
my $last = MT::Fault::last();
print "hb: found=$found cause=$last->{cause} ms=$ms\n";
PERL
    like($out, qr/^hb: found=1 cause=HEARTBEAT_TIMEOUT ms=\d+$/m,
         '6b: stopped heartbeat detected within deadline');
}

# ---- 6. MT::Pool supervision, respawn, backoff & exact counts ----
{
    my $nitems = $long ? 50_000 : 1_500;
    my $out = child(<<"PERL");
use strict; use warnings;
use MT::Pool;
use MT::Fault;

sub flaky_square {
    my (\$x) = \@_;
    # Trigger deterministic worker SIGSEGV on 6 items spread across the run
    if (\$x == 50 || \$x == 250 || \$x == 500 || \$x == 750 || \$x == 1000 || \$x == 1250) {
        MT::Fault::_trigger_sigsegv();
    }
    return \$x * \$x;
}

my %failed;
my \$dup_fail = 0;
my \$pool = MT::Pool->new(
    workers         => 4,
    max_restarts    => 16,
    backoff_base_ms => 5,
    backoff_max_ms  => 50,
    on_fault        => sub {
        my (\$idx, \$rec) = \@_;
        \$dup_fail++ if exists \$failed{\$idx};
        \$failed{\$idx} = \$rec;
    },
);

my \@res = \$pool->map(\\&flaky_square, 1 .. $nitems);
my \$ok = 0;
my \$bad_val = 0;
for my \$i (0 .. \$#res) {
    if (exists \$failed{\$i}) {
        \$bad_val++ if defined \$res[\$i];
    }
    else {
        if (defined \$res[\$i] && \$res[\$i] == (\$i + 1) * (\$i + 1)) {
            \$ok++;
        }
        else {
            \$bad_val++;
        }
    }
}
my \@faults = \$pool->faults;
my \$restarts = \$pool->restarts;
\$pool->shutdown;
print "pool_sup: total=", (\$ok + scalar(keys \%failed)), " want=$nitems",
      " ok=\$ok failed=", scalar(keys \%failed), " dup_fail=\$dup_fail",
      " bad_val=\$bad_val faults=", scalar(\@faults), " restarts=\$restarts\\n";
PERL
    like($out, qr/^pool_sup: total=$nitems want=$nitems ok=@{[$nitems - 6]} failed=6 dup_fail=0 bad_val=0 faults=6 restarts=6$/m,
         "6e: MT::Pool maps $nitems items across worker kills with zero lost/duplicate items");
}

# ---- 7. MT::Pool exponential backoff & max_restarts cap ----------
{
    my $out = child(<<'PERL');
use strict; use warnings;
use MT::Pool;
use MT::Fault;

sub always_crash { MT::Fault::_trigger_sigsegv() }

my $pool = MT::Pool->new(
    workers         => 1,
    max_restarts    => 2,
    backoff_base_ms => 10,
    backoff_max_ms  => 100,
    on_fault        => sub {},
);

eval { $pool->map(\&always_crash, 1, 2, 3, 4) };
my $err = $@;
my @f = $pool->faults;
my @backoffs = map { $_->{backoff_ms} // 0 } grep { !$_->{exhausted} } @f;
my $exhausted = scalar grep { $_->{exhausted} } @f;
my $restarts = $pool->restarts;
$pool->shutdown;
print "backoff: restarts=$restarts exhausted=$exhausted",
      " seq=", join(',', @backoffs),
      " err_ok=", ($err =~ /exhausted restart limit/ ? 1 : 0), "\n";
PERL
    like($out, qr/^backoff: restarts=2 exhausted=1 seq=10,20 err_ok=1$/m,
         '6e: MT::Pool enforces exponential backoff (10ms, 20ms) and max_restarts cap');
}

# ---- 8. named endpoints (MT::Pool->named & MT::Channel->named) ---
{
    my $out = child(<<'PERL');
use strict; use warnings;
use MT::Pool;
use MT::Channel;

sub double_it { return $_[0] * 2 }

sub client_worker {
    my $ev = MT::Channel->named('events');
    $ev->send("hello-from-worker");
    my $p = MT::Pool->named('ingest');
    die "pool not found" unless $p;
    my $val = $p->run(\&double_it, 21)->join;
    return "val=$val";
}

my $ev = MT::Channel->named('events', 8);
my $pool = MT::Pool->new(workers => 2, name => 'ingest');
my $wid = MT::Worker::spawn('main::client_worker');
my $msg = $ev->recv;
my $wres = MT::Worker::join($wid);
my $same_pool = MT::Pool->named('ingest');
my $direct = $same_pool->run(\&double_it, 50)->join;
$pool->shutdown;
my $after_shutdown = MT::Pool->named('ingest');
MT::Channel->unregister_name('events');
print "named: msg=$msg wres=$wres direct=$direct after=", (defined $after_shutdown ? "live" : "gone"), "\n";
PERL
    like($out, qr/^named: msg=hello-from-worker wres=val=42 direct=100 after=gone$/m,
         '6e: named MT::Pool and MT::Channel bind transparently across workers');
}

# ---- 9. explicit checkpoints (in-memory & file-backed) -----------
{
    my $ckpt_file = tempfile();
    my $out = child(<<"PERL");
use strict; use warnings;
use MT::Checkpoint;

# In-memory checkpoint
my \$state = { step => 3000, acc => 4501500, tags => ['a', 'b', undef, 3.5] };
MT::Checkpoint::save(\$state, name => 'mem1');
my \$r1 = MT::Checkpoint::restore(name => 'mem1');
MT::Checkpoint::clear(name => 'mem1');
my \$r1_after = MT::Checkpoint::restore(name => 'mem1');

# File-backed checkpoint
MT::Checkpoint::save(\$state, file => '$ckpt_file');
my \$r2 = MT::Checkpoint::restore(file => '$ckpt_file');
MT::Checkpoint::clear(file => '$ckpt_file');
my \$r2_after = MT::Checkpoint::restore(file => '$ckpt_file');

print "mem: step=\$r1->{step} acc=\$r1->{acc} tag=\$r1->{tags}[0] nv=\$r1->{tags}[3] cleared=",
      (defined \$r1_after ? 0 : 1), "\\n";
print "file: step=\$r2->{step} acc=\$r2->{acc} tag=\$r2->{tags}[1] undef_ok=",
      (!defined \$r2->{tags}[2] ? 1 : 0), " cleared=", (defined \$r2_after ? 0 : 1), "\\n";
PERL
    like($out, qr/^mem: step=3000 acc=4501500 tag=a nv=3\.5 cleared=1$/m,
         '6f: MT::Checkpoint in-memory save/restore/clear preserves nested state');
    like($out, qr/^file: step=3000 acc=4501500 tag=b undef_ok=1 cleared=1$/m,
         '6f: MT::Checkpoint file-backed save/restore/clear preserves nested state');
}

# ---- 10. process pairs (pairs => 1) takeover on primary fault ----
{
    my $out = child(<<'PERL');
use strict; use warnings;
use Time::HiRes qw(time);
use MT::Pool;
use MT::Checkpoint;
use MT::Fault;

sub checkpointed_job {
    my ($spec) = @_;
    my ($total_steps, $crash_at) = split /:/, $spec;
    my $state = MT::Checkpoint::restore() // { step => 0, acc => 0, resumed => 0 };
    $state->{resumed} = 1 if MT::Checkpoint::is_backup();

    while ($state->{step} < $total_steps) {
        $state->{step}++;
        $state->{acc} += $state->{step};
        if ($state->{step} % 1000 == 0) {
            MT::Checkpoint::save($state);
        }
        if ($crash_at > 0 && $state->{step} == $crash_at && !MT::Checkpoint::is_backup()) {
            MT::Fault::_trigger_sigsegv();
        }
    }
    return $state;
}

my $pool = MT::Pool->new(workers => 2, pairs => 1);

# Baseline unkilled run
my $ref = $pool->run(\&checkpointed_job, "5000:0")->join;

# Primary crashes at step 3500 (after checkpoint at 3000); backup resumes and completes
my $t0 = time();
my $res = $pool->run(\&checkpointed_job, "5000:3500")->join;
my $total_ms = sprintf("%.2f", (time() - $t0) * 1000);

my $takeovers = $pool->takeovers;
my $takeover_ms = sprintf("%.2f", $pool->last_takeover_ms);
my @faults = $pool->faults;
$pool->shutdown;

print "pair: ref_acc=$ref->{acc} res_acc=$res->{acc} match=",
      ($ref->{acc} == $res->{acc} && $res->{step} == 5000 ? 1 : 0),
      " resumed=$res->{resumed} takeovers=$takeovers faults=", scalar(@faults),
      " takeover_ms=$takeover_ms total_ms=$total_ms\n";
PERL
    like($out, qr/^pair: ref_acc=12502500 res_acc=12502500 match=1 resumed=1 takeovers=1 faults=1 takeover_ms=[\d.]+ total_ms=[\d.]+$/m,
         '6f: process-pair backup takes over from checkpoint on primary SIGSEGV with exact result');
    note("process pair output:\n$out");
}

# ---- 11. paired pool map with multiple primary kills (0 job loss)-
{
    my $out = child(<<'PERL');
use strict; use warnings;
use MT::Pool;
use MT::Checkpoint;
use MT::Fault;

sub paired_item {
    my ($id) = @_;
    my $st = MT::Checkpoint::restore() // { id => $id, step => 0, sum => 0 };
    while ($st->{step} < 2000) {
        $st->{step}++;
        $st->{sum} += $st->{step} + $id;
        MT::Checkpoint::save($st) if $st->{step} % 1000 == 0;
        if (($id == 3 || $id == 7 || $id == 11) && $st->{step} == 1500 && !MT::Checkpoint::is_backup()) {
            MT::Fault::_trigger_sigsegv();
        }
    }
    return $st->{sum};
}

my $pool = MT::Pool->new(workers => 2, pairs => 1, max_restarts => 10, backoff_base_ms => 5);
my @got = $pool->map(\&paired_item, 1 .. 15);
my $bad = 0;
for my $id (1 .. 15) {
    my $want = 2000 * 2001 / 2 + 2000 * $id;
    $bad++ unless defined $got[$id - 1] && $got[$id - 1] == $want;
}
my $takeovers = $pool->takeovers;
$pool->shutdown;
print "pair_map: n=", scalar(@got), " bad=$bad takeovers=$takeovers\n";
PERL
    like($out, qr/^pair_map: n=15 bad=0 takeovers=3$/m,
         '6f: paired MT::Pool->map survives 3 primary SIGSEGVs with 0 lost jobs');
}

# ---- 12. benchmark Workload.pm sanity check ----------------------
{
    my $out = child(<<'PERL');
use strict; use warnings;
use lib '../tools/bench';
use Workload qw(setup item physical_cores rss_kb rss_per_worker_kb image_mb);
my $cores = physical_cores();
my $dir = $ENV{TMPDIR} || '/tmp';
my $file = setup($dir, image_mb => 2);
my $s1 = item(1);
my $s2 = item(1);
unlink $file;
print "bench: cores=$cores img_mb=", image_mb(), " deterministic=", ($s1 == $s2 ? 1 : 0),
      " rss_ok=", (rss_kb($$) > 0 ? 1 : 0), "\n";
PERL
    like($out, qr/^bench: cores=[1-9]\d* img_mb=2 deterministic=1 rss_ok=1$/m,
         '6h: reworked tools/bench Workload.pm preloads image and runs deterministically');
}

# ---- 13. Extra assertions on fault records & supervisor status -------------
{
    my $out = child(<<'PERL');
use strict; use warnings;
use MT::Fault;
use MT::Supervisor;
use MT::Pool;

sub ok_sub { return $_[0] + 1 }
my $pool = MT::Pool->new(workers => 2);
my $r = $pool->run(\&ok_sub, 41)->join;
$pool->map(\&ok_sub, 1 .. 10);
my $st = MT::Supervisor::status();
my @live_workers = grep { $_->{unit_kind} eq 'worker' && $_->{state} eq 'live' } @$st;
my $alive = $pool->workers_alive;
$pool->shutdown;
print "sup_pool: r=$r alive=$alive tracked=", scalar(@live_workers), "\n";
PERL
    like($out, qr/^sup_pool: r=42 alive=2 tracked=2$/m,
         '6b+6e: MT::Pool workers are tracked live by MT::Supervisor');
}

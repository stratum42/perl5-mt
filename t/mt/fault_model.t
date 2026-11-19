#!./perl
# perl5-mt: fail-fast policy, fault event model (MT::Fault),
# and supervisor liveness heartbeats over krt_vec.h (MT::Supervisor).

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib', '../ext/MT/lib');
}

use strict;
use warnings;

plan(tests => 14);

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

# ---- 1. Classification table (MT::Fault::sites) ----------------------------
{
    my $out = child(<<'PERL');
use strict; use warnings;
use MT::Fault;
my $sites = MT::Fault::sites();
my $n = scalar @$sites;
my $rec = grep { $_->{severity} eq 'recoverable' } @$sites;
my $fat = grep { $_->{severity} eq 'fatal' } @$sites;
my %files = map { $_->{file} => 1 } @$sites;
print "n=$n rec=$rec fat=$fat files=", join(',', sort keys %files), "\n";
PERL
    like($out, qr/n=(\d+) rec=(\d+) fat=(\d+)/, 'MT::Fault::sites returns classified table');
    like($out, qr/mt_cap\.c.*mt_chan\.c.*mt_fiber\.c.*mt_freeze\.c.*mt_worker\.c/,
         'classification covers core mt_*.c files');
    if ($out =~ /rec=(\d+) fat=(\d+)/) {
        ok($1 > 0 && $2 > 0, "both recoverable ($1) and fatal ($2) sites classified");
    }
    else {
        fail('site counts parsed');
    }
}

# ---- 2. Recoverable fault injection, MT::Fault::last & MT::Fault::on -------
{
    my $out = child(<<'PERL');
use strict; use warnings;
use MT::Fault;
MT::Fault::reset();
print "before=", (defined MT::Fault::last() ? "defined" : "undef"), "\n";
my @seen;
MT::Fault::on(sub {
    my ($f) = @_;
    push @seen, "$f->{seq}:$f->{cause}:$f->{severity}:$f->{unit_kind}:$f->{message}";
});
eval { MT::Fault::_inject_recoverable("synthetic unit fault") };
my $err = $@;
my $dispatched = MT::Fault::poll();
my $last = MT::Fault::last();
print "caught=", ($err =~ /synthetic unit fault/ ? "yes" : "no"), "\n";
print "dispatched=$dispatched seen=", join('|', @seen), "\n";
print "last=$last->{seq}:$last->{cause}:$last->{severity}:$last->{message}\n";
PERL
    like($out, qr/^before=undef$/m, 'MT::Fault::last is undef before any fault');
    like($out, qr/^caught=yes$/m, 'recoverable fault croaks cleanly without killing process');
    like($out, qr/^dispatched=1 seen=1:TEST_RECOVERABLE:recoverable:main:synthetic unit fault$/m,
         'MT::Fault::on observer receives recoverable fault event');
    like($out, qr/^last=1:TEST_RECOVERABLE:recoverable:synthetic unit fault$/m,
         'MT::Fault::last returns the recorded fault');
}

# ---- 3. Fatal fault injection terminates process fail-fast -----------------
{
    my $code = <<'PERL';
BEGIN { unshift @INC, '../ext/MT/lib' }
use strict; use warnings;
use MT::Fault;
MT::Fault::_inject_fatal("synthetic host invariant failure");
print "UNREACHABLE\n";
PERL
    my $file = tempfile();
    open my $fh, ">", $file or die "open $file: $!";
    print $fh $code;
    close $fh or die "close $file: $!";
    my $out = runperl(progfile => $file, stderr => 1);
    my $rc = $?;
    ok($rc != 0, 'fatal fault exits with non-zero status');
    like($out, qr/FATAL fault #1 \[TEST_FATAL\].*synthetic host invariant failure/,
         'fatal fault writes host-visible diagnostic to stderr');
    unlike($out, qr/UNREACHABLE/, 'fatal fault terminates immediately (fail-fast)');
}

# ---- 4. Worker error records recoverable fault -----------------------------
{
    my $out = child(<<'PERL');
use strict; use warnings;
use MT::Fault;
sub failing_worker { die "boom in worker\n" }
MT::Image::freeze();
MT::Fault::reset();
my @seen;
MT::Fault::on(sub { push @seen, $_[0] });
my $id = MT::Worker::spawn('main::failing_worker');
eval { MT::Worker::join($id) };
my $err = $@;
MT::Fault::poll();
my $last = MT::Fault::last();
print "err=", ($err =~ /boom in worker/ ? "yes" : "no"), "\n";
print "seen=", scalar(@seen), " kind=$last->{unit_kind} id=$last->{unit_id} sev=$last->{severity}\n";
PERL
    like($out, qr/^err=yes$/m, 'worker die propagated to join');
    like($out, qr/^seen=1 kind=worker id=1 sev=recoverable$/m,
         'worker die recorded as recoverable worker fault and observed');
}

# ---- 5. Heartbeat timeout detection via krt_vec.h & MT::Supervisor ---------
{
    local $ENV{PERL_MT_HEARTBEAT_MS}   = 25;
    local $ENV{PERL_MT_HEARTBEAT_MISS} = 3;
    my $out = child(<<'PERL');
use strict; use warnings;
use Time::HiRes qw(time sleep);
use MT::Fault;
use MT::Supervisor;
use MT::Channel;

sub sleepy_worker {
    my ($in_id, $out_id) = @_;
    my $in  = MT::Channel->attach($in_id);
    my $out = MT::Channel->attach($out_id);
    MT::Supervisor::beat();
    $out->send("started");
    # Wait until told to pause heartbeat
    $in->recv();
    MT::Supervisor::_pause_heartbeat(1);
    $out->send("paused");
    # Stay alive until main tells us to exit
    $in->recv();
    return "done";
}

MT::Image::freeze();
MT::Fault::reset();

my @faults;
MT::Fault::on(sub { push @faults, $_[0] });

my $to_w   = MT::Channel->new(4);
my $from_w = MT::Channel->new(4);
my $wid = MT::Worker::spawn('main::sleepy_worker', $to_w->id, $from_w->id);
die "expected started" unless $from_w->recv() eq 'started';

# Healthy tick: worker should be live
MT::Supervisor::tick();
my $st = MT::Supervisor::status();
my ($w_st) = grep { $_->{unit_kind} eq 'worker' && $_->{unit_id} == $wid } @$st;
print "initial_state=", ($w_st ? $w_st->{state} : "missing"), "\n";

# Tell worker to stop posting heartbeats and measure detection deadline
$to_w->send("pause_now");
die "expected paused" unless $from_w->recv() eq 'paused';

my $t0 = time();
my $detected = 0;
while (time() - $t0 < 2.0) {
    my $n = MT::Supervisor::tick();
    if ($n > 0) {
        $detected = 1;
        last;
    }
    sleep(0.01);
}
my $elapsed_ms = int((time() - $t0) * 1000);
$to_w->send("exit_now");
MT::Worker::join($wid);

my $st2 = MT::Supervisor::status();
my ($w_st2) = grep { $_->{unit_kind} eq 'worker' && $_->{unit_id} == $wid } @$st2;
my $hb_fault = (grep { $_->{cause} eq 'HEARTBEAT_TIMEOUT' } @faults)[0];

# Measure single-store beat cost
my $iters = 100_000;
my $ns = MT::Supervisor::_bench_beat($iters);
my $ns_per_beat = sprintf("%.1f", $ns / $iters);

print "detected=$detected elapsed_ms=$elapsed_ms final_state=$w_st2->{state}\n";
print "fault=", ($hb_fault ? "$hb_fault->{cause}:$hb_fault->{unit_kind}:$hb_fault->{unit_id}" : "none"), "\n";
print "beat_ns=$ns_per_beat\n";
PERL
    like($out, qr/^initial_state=live$/m, 'live worker reported in MT::Supervisor::status');
    like($out, qr/^detected=1 elapsed_ms=(\d+) final_state=faulted$/m,
         'paused worker heartbeat detected and marked faulted within deadline');
    note("supervisor output:\n$out");
}

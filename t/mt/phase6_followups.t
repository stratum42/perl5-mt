#!./perl
# perl5-mt: fault containment and worker pool regression tests:
#   (1) MT::Pool nested data and capability marshaling
#   (2) MT::Cell lock-free compare-and-swap on the body pointer
#   (3) Channel cross-worker wake through the receiving worker's slot vector
#   (4) Combined aio + doorbell wait (IORING_OP_FUTEX_WAIT / PERL_KRT_ENOSYS)

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib', '../ext/MT/lib');
}

use strict;
use warnings;

plan(tests => 11);

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

# ---- (1) MT::Pool nested data & capability marshaling ----------------------
{
    my $out = child(<<'PERL');
use strict; use warnings;
use MT::Pool;

sub transform {
    my ($payload) = @_;
    my $cap_val = MT::Cap::get($payload->{cap});
    my $sum = 0;
    $sum += $_ for @{ $payload->{nums} };
    my $out_cap = MT::Cap::mint("worker:" . $cap_val);
    return {
        sum     => $sum,
        nested  => [ $payload->{meta}{tag}, undef, 3.5, ${ $payload->{sref} } ],
        cap_out => $out_cap,
    };
}

sub map_nested {
    my ($item) = @_;
    return { id => $item->{id}, val => $item->{v} * 2 };
}

MT::Image::freeze();
my $pool = MT::Pool->new(workers => 4);

my $in_cap = MT::Cap::mint("secret-42");
my $scalar = "hello-ref";
my $job = $pool->run(\&transform, {
    nums => [10, 20, 30],
    meta => { tag => "ok-utf8-\x{2603}" },
    sref => \$scalar,
    cap  => $in_cap,
});
my $res = $job->join;

print "sum=$res->{sum}\n";
print "undef_ok=", (!defined($res->{nested}[1]) ? 1 : 0), "\n";
print "nv=$res->{nested}[2]\n";
print "sref=$res->{nested}[3]\n";
print "utf8_ok=", ($res->{nested}[0] eq "ok-utf8-\x{2603}" ? 1 : 0), "\n";
print "cap_out=", MT::Cap::get($res->{cap_out}), "\n";

# map over nested hashes
my @mapped = $pool->map(\&map_nested, map { { id => $_, v => $_ + 1 } } 1 .. 20);
my $map_ok = 1;
for my $i (0 .. 19) {
    $map_ok = 0 unless $mapped[$i]{id} == $i + 1 && $mapped[$i]{val} == ($i + 2) * 2;
}
print "map_ok=$map_ok\n";

# unshareable CODE ref is still refused cleanly
eval { $pool->run(\&transform, sub { 1 }) };
print "code_refused=", ($@ =~ /MT_UNSHAREABLE_REF/ ? 1 : 0), "\n";

$pool->shutdown;
PERL
    like($out, qr/^sum=60$/m, 'MT::Pool marshals nested array/hash arguments and results');
    like($out, qr/^undef_ok=1$/m, 'MT::Pool preserves undef in nested structures');
    like($out, qr/^nv=3\.5$/m, 'MT::Pool preserves NV in nested structures');
    like($out, qr/^sref=hello-ref$/m, 'MT::Pool marshals scalar references');
    like($out, qr/^utf8_ok=1$/m, 'MT::Pool preserves UTF-8 strings');
    like($out, qr/^cap_out=worker:secret-42$/m, 'MT::Pool marshals MT::Cap handles in both directions');
    like($out, qr/^map_ok=1$/m, 'MT::Pool->map handles nested data structures in order');
    like($out, qr/^code_refused=1$/m, 'MT::Pool refuses unshareable CODE refs with MT_UNSHAREABLE_REF');
}

# ---- (2) Lock-free CAS MT::Cell under multi-worker contention --------------
{
    my $out = child(<<'PERL');
use strict; use warnings;
use Time::HiRes qw(time);
use MT::Cell;

sub bump_cell {
    my ($cid, $n) = @_;
    my $c = MT::Cell->attach($cid);
    for (1 .. $n) {
        $c->update(sub { $_[0] + 1 });
    }
    return $n;
}

MT::Image::freeze();
my $cell = MT::Cell->new(0);
my $workers = 8;
my $per_worker = 1000;
my $t0 = time();
my @ids = map { MT::Worker::spawn('main::bump_cell', $cell->id, $per_worker) } 1 .. $workers;
MT::Worker::join($_) for @ids;
my $dt_ms = sprintf("%.2f", (time() - $t0) * 1000);
my $final = $cell->get;
print "final=$final expected=", ($workers * $per_worker), " dt_ms=$dt_ms\n";
PERL
    like($out, qr/^final=8000 expected=8000 /m,
         'lock-free CAS MT::Cell exact under 8-worker contention');
    note("cell CAS output: $out");
}

# ---- (3) Channel cross-worker wake via slot vector -------------------------
{
    my $out = child(<<'PERL');
use strict; use warnings;
use Time::HiRes qw(time);
use MT::Channel;

sub pong_worker {
    my ($in_id, $out_id, $rounds) = @_;
    my $in  = MT::Channel->attach($in_id);
    my $out = MT::Channel->attach($out_id);
    for (1 .. $rounds) {
        my $v = $in->recv;
        $out->send($v + 1);
    }
    return 1;
}

MT::Image::freeze();
my $c_req = MT::Channel->new(1);
my $c_rep = MT::Channel->new(1);
my $rounds = 2000;
my $wid = MT::Worker::spawn('main::pong_worker', $c_req->id, $c_rep->id, $rounds);

my $t0 = time();
my $v = 0;
for (1 .. $rounds) {
    $c_req->send($v);
    $v = $c_rep->recv;
}
my $elapsed_us = (time() - $t0) * 1_000_000;
my $us_per_rt = sprintf("%.2f", $elapsed_us / $rounds);
MT::Worker::join($wid);
print "rounds=$v us_per_rt=$us_per_rt\n";
PERL
    like($out, qr/^rounds=2000 us_per_rt=/m,
         'channel cross-worker slot-vector wake completes 2000 ping-pongs');
    note("channel wake output: $out");
}

# ---- (4) Combined aio + doorbell sleep / PERL_KRT_ENOSYS fallback ----------
{
    my $out = child(<<'PERL');
use strict; use warnings;
use MT;
my $rc = MT::IO::_test_combined_sleep();
print "combined_rc=$rc os=$^O\n";
PERL
    like($out, qr/^combined_rc=(?:0|-38) os=/m,
         'combined aio+doorbell sleep succeeds (0) or returns PERL_KRT_ENOSYS (-38)');
    note("combined sleep output: $out");
}

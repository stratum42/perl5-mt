#!./perl
# perl5-mt: capabilities (MT::Cap): mint, transfer between
# workers, drop, DESTROY routed to the owner, stale handles, exact counts.

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib');
}

use strict;
use warnings;

plan(tests => 28);

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

# ---- in the main interpreter ------------------------------------------------

{
    my $c = MT::Cap::mint("hello");
    ok(MT::Cap::is_cap($c), 'mint returns a capability handle');
    isa_ok($c, 'MT::Cap');
    is(MT::Cap::get($c), "hello", 'get returns the value');
    is(MT::Cap::count($c), 1, 'one handle');
    my $n = MT::Cap::mint(42);
    is(MT::Cap::get($n), 42, 'integers round-trip');
    my $f = MT::Cap::mint(2.5);
    is(MT::Cap::get($f), 2.5, 'floats round-trip');
    my $u = MT::Cap::mint(undef);
    ok(!defined MT::Cap::get($u), 'undef round-trips');
    ok(!MT::Cap::is_cap("plain"), 'a plain scalar is not a handle');
    eval { MT::Cap::mint([1]) };
    like($@, qr/MT_UNSHAREABLE_REF/, 'a reference cannot be minted');

    my $s0 = MT::Cap::stats();
    is($s0->{used}, 4, 'four slots in use');
    MT::Cap::drop($c);
    eval { MT::Cap::get($c) };
    like($@, qr/MT_STALE_CAP/, 'a dropped handle is stale');
    is(MT::Cap::reap(), 1, 'the owner reaps the destroyed capability');
    my $s1 = MT::Cap::stats();
    is($s1->{used}, 3, '... and the slot is free again');
    is($s1->{destroyed}, 1, 'destroyed counts it');
    undef $n; undef $f; undef $u;
    is(MT::Cap::reap(), 3, 'handles going out of scope destroy their capabilities');
    is(MT::Cap::stats()->{used}, 0, 'no slot leaked');
}

# ---- across workers ------------------------------------------------------

{
    my $out = child(<<'CODE');
sub work {
    my ($cap, $n) = @_;
    my $v = MT::Cap::get($cap);           # a handle that crossed over
    my $sum = 0;
    for my $i (1 .. $n) {
        my $c = MT::Cap::mint($i);        # mint ...
        $sum += MT::Cap::get($c);         # ... use ...
        MT::Cap::drop($c) if $i % 2;      # ... drop half explicitly, the
    }                                     # other half when $c goes away
    MT::Cap::reap();
    my $back = MT::Cap::mint("from worker $v");
    return $back;                         # transfer a handle to the parent
}
MT::Image::freeze();
my $shared = MT::Cap::mint("shared");
my @ids = map { MT::Worker::spawn('main::work', $shared, 10_000) } 1 .. 16;
my @back = map { MT::Worker::join($_) } @ids;
print "count=", MT::Cap::count($shared), "\n";   # only the parent's handle is left
print "back=", join(",", sort map { MT::Cap::get($_) } @back), "\n";
undef @back;
undef $shared;
my $reaped = MT::Cap::reap();
my $s = MT::Cap::stats();
print "reaped=$reaped used=$s->{used} minted=$s->{minted} destroyed=$s->{destroyed} rerouted=$s->{rerouted}\n";
CODE
    like($out, qr/^count=1$/m, 'the workers gave their copies of the shared handle back');
    like($out, qr/^back=from worker shared(,from worker shared){15}$/m,
         'sixteen handles minted in workers were transferred to the parent');
    like($out, qr/^reaped=17 used=0 minted=160017 destroyed=160017 rerouted=16$/m,
         '160k mint/transfer/drop: no slot leaked, DESTROY counts exact, dead owners rerouted');
}

# ---- a stale handle to a reused slot ----------------------------------------

{
    my $out = child(<<'CODE');
my $a = MT::Cap::mint("a");
MT::Cap::drop($a);
MT::Cap::reap();
my $b = MT::Cap::mint("b");               # reuses the slot, new generation
eval { MT::Cap::get($a) };
print "stale: $@";
print "b=", MT::Cap::get($b), "\n";
CODE
    like($out, qr/^stale: .*MT_STALE_CAP/m, 'a handle to a reused slot is stale');
    like($out, qr/^b=b$/m, '... and the new capability is intact');
}

# ---- forced worker deaths while holding capabilities -----------------------

{
    my $out = child(<<'CODE');
sub doomed { my ($cap) = @_; my @mine = map { MT::Cap::mint($_) } 1 .. 100; die "killed\n" }
sub quitter { my ($cap) = @_; my $c = MT::Cap::mint("x"); exit 3 }
MT::Image::freeze();
my $shared = MT::Cap::mint("shared");
my @ids = ((map { MT::Worker::spawn('main::doomed', $shared) } 1 .. 4),
           (map { MT::Worker::spawn('main::quitter', $shared) } 1 .. 2));
my @err;
for (@ids) { my $r = eval { MT::Worker::join($_) }; push @err, $@ || "ok" }
print "errors=", scalar(grep { /killed|exit\(\) called/ } @err), "\n";
print "count=", MT::Cap::count($shared), "\n";
undef $shared;
my $reaped = MT::Cap::reap();
my $s = MT::Cap::stats();
print "reaped=$reaped used=$s->{used} minted=$s->{minted} destroyed=$s->{destroyed}\n";
CODE
    like($out, qr/^errors=6$/m, 'all six workers died or exited');
    like($out, qr/^count=1$/m, 'their handles to the shared capability were released');
    like($out, qr/^reaped=1 used=0 minted=403 destroyed=403$/m,
         'every capability of a dead worker was reclaimed, none leaked');
}

# ---- the parent is told about capabilities its dead workers minted ----------

{
    my $out = child(<<'CODE');
sub maker { return MT::Cap::mint("made in worker") }
MT::Image::freeze();
my $c = MT::Worker::join(MT::Worker::spawn('main::maker'));
print "value=", MT::Cap::get($c), " count=", MT::Cap::count($c), "\n";
undef $c;
print "reaped=", MT::Cap::reap(), " rerouted=", MT::Cap::stats()->{rerouted}, " used=", MT::Cap::stats()->{used}, "\n";
CODE
    like($out, qr/^value=made in worker count=1$/m, 'a capability minted in a worker outlives it');
    like($out, qr/^reaped=1 rerouted=1 used=0$/m, 'its DESTROY was rerouted to the parent and reaped there');
}

# ---- ten million operations -------------------------------------------------

SKIP: {
    skip 'set PERL_MT_LONG_TESTS to run the 10M-operation case', 2
        unless $ENV{PERL_MT_LONG_TESTS};
    my $out = child(<<'CODE');
sub churn { my ($n) = @_; for my $i (1 .. $n) { my $c = MT::Cap::mint($i); MT::Cap::get($c); MT::Cap::drop($c) } MT::Cap::reap(); return $n }
MT::Image::freeze();
my @ids = map { MT::Worker::spawn('main::churn', 208_334) } 1 .. 16;
my $n = 0; $n += MT::Worker::join($_) for @ids;
my $s = MT::Cap::stats();
print "ops=", 3 * $n, " used=$s->{used} minted=$s->{minted} destroyed=$s->{destroyed}\n";
CODE
    like($out, qr/^ops=10000032 used=0 /m, 'ten million mint/get/drop operations, no slot leaked');
    like($out, qr/ minted=3333344 destroyed=3333344$/m, '... exact DESTROY counts');
}

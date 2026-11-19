#!./perl
# perl5-mt: the worker pool, channels, cells and shared hashes
# (mt_chan.c, ext/MT).  A 16-worker pool mapping 100k items in order;
# producers and consumers moving values through a channel with exact counts;
# a cell updated by 16 workers with an exact final count; a shared hash
# written by workers; the shutdown at exit with a job still running; the
# refusal of a mutable reference; and the MT::IO wrappers.  Each case runs in
# a child perl: a frozen image is process-wide.  PERL_MT_LONG_TESTS=1 runs the
# full stress sizes.

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib', '../ext/MT/lib');
}

use strict;
use warnings;

my $long = $ENV{PERL_MT_LONG_TESTS};
plan(tests => 13);

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

my $prelude = <<'PRELUDE';
use strict; use warnings; $| = 1;
use MT::Pool; use MT::Channel; use MT::Cell; use MT::SharedHash;
PRELUDE

# ---- a 16-worker pool maps the items in order ----------------------------

{
    my $n = $long ? 100_000 : 10_000;
    my $out = child($prelude . <<"CODE");
sub square { \$_[0] * \$_[0] }
my \$pool = MT::Pool->new(workers => 16);
my \@r = \$pool->map(\\&square, 1 .. $n);
my \$bad = 0;
for my \$i (0 .. \$#r) { \$bad++ unless \$r[\$i] == (\$i + 1) * (\$i + 1) }
print "map: n=", scalar(\@r), " bad=\$bad\\n";
\$pool->shutdown;
CODE
    like($out, qr/^map: n=$n bad=0$/m, "a 16-worker pool maps $n items, in order");
}

# ---- run, join and an error in a job --------------------------------------

{
    my $out = child($prelude . <<'CODE');
sub work { die "bad item $_[0]\n" if $_[0] == 3; return "ok$_[0]" }
my $pool = MT::Pool->new(workers => 2);
print "run: ", $pool->run(\&work, 7)->join, "\n";
my $e = eval { $pool->run(\&work, 3)->join; 1 } ? "none" : $@;
print "err: $e";
$pool->shutdown;
print "shut: ", $pool->shutdown, "\n";
CODE
    like($out, qr/^run: ok7$/m, 'run returns the result of the job');
    like($out, qr/^err: bad item 3$/m, 'an error in a job is rethrown by join');
}

# ---- producers and consumers: exact counts through a channel ---------------

{
    my $n = $long ? 250_000 : 20_000;     # per producer: 4 producers
    my $out = child($prelude . <<"CODE");
sub produce { my (\$id, \$k) = \@_; my \$ch = MT::Channel->attach(\$id); \$ch->send(\$k * 1_000_000 + \$_) for 1 .. $n; return 1 }
sub consume {
    my (\$id) = \@_; my \$ch = MT::Channel->attach(\$id); my (\$count, \$sum) = (0, 0);
    while (defined(my \$v = \$ch->recv)) { \$count++; \$sum += \$v }
    return "\$count:\$sum";
}
my \$ch = MT::Channel->new(256);
my \$res = MT::Channel->new(8);
MT::Image::freeze();
my \@p = map { MT::Worker::spawn('main::produce', \$ch->id, \$_) } 1 .. 4;
my \@c = map { MT::Worker::spawn('main::consume', \$ch->id) } 1 .. 4;
MT::Worker::join(\$_) for \@p;
\$ch->close;
my \$count = 0; my \$sum = 0;
for my \$id (\@c) {
    my (\$c, \$s) = split /:/, MT::Worker::join(\$id);
    \$count += \$c; \$sum += \$s;
}
my \$want = 0;
for my \$k (1 .. 4) { \$want += \$k * 1_000_000 * $n + $n * ($n + 1) / 2 }
print "chan: count=\$count want=", 4 * $n, " sum_ok=", (\$sum == \$want ? 1 : 0), "\\n";
CODE
    like($out, qr/^chan: count=@{[4 * $n]} want=@{[4 * $n]} sum_ok=1$/m,
         "4 producers and 4 consumers move $n values each, exact counts and sum");
}

# ---- a cell: 16 workers, exact final count --------------------------------

{
    my $n = $long ? 1_000_000 : 100_000;
    my $per = $n / 16;
    my $out = child($prelude . <<"CODE");
sub bump { my \$cell = MT::Cell->attach(\$_[0]); \$cell->update(sub { \$_[0] + 1 }) for 1 .. $per; return 1 }
my \$cell = MT::Cell->new(0);
MT::Image::freeze();
my \@ids = map { MT::Worker::spawn('main::bump', \$cell->id) } 1 .. 16;
MT::Worker::join(\$_) for \@ids;
print "cell: ", \$cell->get, "\\n";
CODE
    like($out, qr/^cell: $n$/m, "16 workers increment a cell $n times in all, exact");
}

# ---- a shared hash written by the workers -----------------------------------

{
    my $n = $long ? 100_000 : 10_000;
    my $out = child($prelude . <<"CODE");
sub fill { my \$h = MT::SharedHash->attach(\$_[0]); my \$lo = \$_[1]; \$h->set("k\$_" => \$_) for \$lo .. \$lo + $n / 4 - 1; return 1 }
my \$h = MT::SharedHash->new;
MT::Image::freeze();
my \@ids = map { MT::Worker::spawn('main::fill', \$h->id, \$_ * ($n / 4)) } 0 .. 3;
MT::Worker::join(\$_) for \@ids;
my \@k = \$h->keys;
print "hash: keys=", scalar(\@k), " k7=", \$h->get('k7'), "\\n";
CODE
    like($out, qr/^hash: keys=$n k7=7$/m, "4 workers write $n keys into a shared hash");
}

# ---- a job still running at exit: no panic, and the warning ----------------

{
    my $out = child(<<'CODE');
use strict; use warnings; $| = 1;
use MT::Pool;
sub slow { select(undef, undef, undef, 1); return 1 }
my $pool = MT::Pool->new(workers => 2);
$pool->run(\&slow);
print "main: done\n";
CODE
    like($out, qr/^main: done$/m, 'a pool with a job running at exit: the program finishes');
    like($out, qr/MT::Pool: 2 worker\(s\) still running at exit/,
         'the END block shuts the pool down with a warning naming the workers');
}

# ---- a mutable reference croaks --------------------------------------------

{
    my $out = child($prelude . <<'CODE');
my $ch = MT::Channel->new(1);
eval { $ch->send([ \my $x ]); 1 } or print "ref: ", ($@ =~ /MT_UNSHAREABLE_REF/ ? "refused" : "other: $@"), "\n";
my $c = MT::Cell->new(1);
eval { $c->set({}); 1 } or print "cell: ", ($@ =~ /MT_UNSHAREABLE_REF/ ? "refused" : "other: $@"), "\n";
CODE
    like($out, qr/^ref: refused$/m, 'sending a reference to a mutable value croaks MT_UNSHAREABLE_REF');
    like($out, qr/^cell: refused$/m, 'setting a cell to a reference croaks MT_UNSHAREABLE_REF');
}

# ---- MT::IO wrappers: the same answers as the core calls -------------------

{
    my $out = child(<<'CODE');
use strict; use warnings;
my $f = ($ENV{TMPDIR} || '/tmp') . "/pool_io_$$";
open my $w, '>:unix', $f or die; print $w "abc" x 1000; close $w;
my @a = MT::IO::stat($f); my @b = stat $f;
print "stat: ", (join(',', @a[0 .. 4, 7]) eq join(',', @b[0 .. 4, 7]) ? "same" : "differ"), "\n";
open my $in, '<:async', $f or die; open my $out, '>:unix', "$f.2" or die;
my $moved = MT::IO::splice($in, $out, 3000);
close $out; close $in;
print "splice: $moved\n";
unlink $f, "$f.2";
CODE
    like($out, qr/^stat: same$/m, 'MT::IO::stat agrees with stat');
    like($out, qr/^splice: 3000$/m, 'MT::IO::splice moves the bytes asked for');
}

# ---- the shutdown returns; the channel and cell objects are shared -------

{
    my $out = child($prelude . <<'CODE');
my $ch = MT::Channel->new(4);
my $again = MT::Channel->attach($ch->id);
$ch->send("shared");
print "attach: ", $again->recv, "\n";
CODE
    like($out, qr/^attach: shared$/m, 'a channel attached by id is the same channel');
}

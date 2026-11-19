#!./perl
# perl5-mt: the :async PerlIO layer over the krt aio ring
# (perlio_async.c).  Byte parity with :unix for reads, writes, appends, seeks,
# large files, utf8 stacking, binmode, pipes and socketpairs; $! on the
# failing opens; MT::IO::stat, splice, fsync and aio_stats; fibers that park on
# their reads; and the ring in workers.  The plain cases run in this process;
# the fiber and worker cases run in child perls, as fiber.t does (a frozen
# image is process-wide).

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib');
}

use strict;
use warnings;
use Errno qw(ENOENT EACCES);
use Time::HiRes qw(time);
use Socket qw(AF_UNIX SOCK_STREAM PF_UNSPEC);

plan(tests => 32);

my $long = $ENV{PERL_MT_LONG_TESTS};

sub slurp {
    my ($layers, $file) = @_;
    open my $fh, "<$layers", $file or die "open $file ($layers): $!";
    binmode $fh;
    local $/;
    my $data = <$fh>;
    close $fh or die "close $file: $!";
    return $data;
}

sub spew {
    my ($layers, $file, $data) = @_;
    open my $fh, ">$layers", $file or die "open $file ($layers): $!";
    binmode $fh;
    print $fh $data or die "print $file: $!";
    close $fh or die "close $file: $!";
}

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

my $file = tempfile();
my $text = join '', map { "line $_ of the parity file\n" } 1 .. 400;

# ---- write and read: the same bytes as :unix --------------------------------

spew(':async', $file, $text);
is(-s $file, length $text, ':async writes every byte of the file');
is(slurp(':async', $file), slurp(':unix', $file), ':async reads what :unix reads');
is(slurp(':async', $file), $text, ':async reads back the bytes it wrote');

# ---- append -----------------------------------------------------------------

{
    open my $fh, '>>:async', $file or die "open append: $!";
    print $fh "tail\n" or die "print: $!";
    close $fh or die "close: $!";
    is(slurp(':unix', $file), $text . "tail\n", '>>:async appends at the end');
}

# ---- seek, tell, eof ----------------------------------------------------------

{
    spew(':async', $file, '0123456789');
    open my $fh, '<:async', $file or die "open: $!";
    is(read($fh, my $a, 3), 3, 'read 3 bytes');
    is(tell($fh), 3, 'tell after a read');
    seek($fh, 5, 0) or die "seek: $!";
    read($fh, my $b, 2);
    is($b, '56', 'seek SEEK_SET, then read');
    seek($fh, -2, 2) or die "seek end: $!";
    read($fh, my $c, 10);
    is($c, '89', 'seek SEEK_END');
    ok(eof($fh), 'eof at the end');
    close $fh;
}

# ---- utf8 stacking --------------------------------------------------------------

{
    my $wide = "caf\x{e9} \x{263a} \x{1F600}\n";
    open my $w, '>:async:utf8', $file or die "open: $!";
    print $w $wide;
    close $w;
    open my $r, '<:async:utf8', $file or die "open: $!";
    my $back = <$r>;
    close $r;
    is($back, $wide, ':async:utf8 stacks with the encoding layer');
}

# ---- binmode onto an open handle --------------------------------------------

{
    spew(':unix', $file, "binmode data\n");
    open my $fh, '<', $file or die "open: $!";
    binmode($fh, ':async') or die "binmode: $!";
    my $got = <$fh>;
    close $fh;
    is($got, "binmode data\n", 'binmode(:async) on an open :unix handle reads through the ring');
}

# ---- a large file, crossing the buffers ------------------------------------------

my $big = "0123456789abcdef" x (16 * 1024 * 1024 / 16);
{
    my $t0 = time;
    spew(':async', $file, $big);
    my $tw = time - $t0;
    $t0 = time;
    my $unix = slurp(':unix', $file);
    my $tu = time - $t0;
    $t0 = time;
    my $async = slurp(':async', $file);
    my $ta = time - $t0;
    ok($async eq $big && $unix eq $big, '16 MiB round trip through :async matches the bytes written');
    printf "# :async read %.1f MB/s, :unix read %.1f MB/s, :async write %.1f MB/s\n",
        16 / $ta, 16 / $tu, 16 / $tw;
}

# ---- pipes and socketpairs ------------------------------------------------------

{
    pipe(my $pr, my $pw) or die "pipe: $!";
    print $pw "through a pipe\n";
    close $pw;
    open my $in, '<&=', fileno($pr) or die "fdopen: $!";
    binmode($in, ':async');
    is(scalar(<$in>), "through a pipe\n", 'a pipe read through :async');
    close $in;
}

{
    socketpair(my $sa, my $sb, AF_UNIX, SOCK_STREAM, PF_UNSPEC) or die "socketpair: $!";
    syswrite($sa, "socket data\n");
    close $sa;
    open my $in, '<&=', fileno($sb) or die "fdopen: $!";
    binmode($in, ':async');
    is(scalar(<$in>), "socket data\n", 'a connected socket read through :async');
    close $in;
}

# ---- errors set $! ------------------------------------------------------------------

{
    ok(!open(my $fh, '<:async', '/nonexistent/perl5-mt/file'), 'open of a missing file fails');
    is($! + 0, ENOENT, '$! is ENOENT for a missing file');
}

SKIP: {
    skip("root reads any file", 2) if $> == 0;
    spew(':unix', $file, "secret\n");
    chmod 0000, $file or die "chmod: $!";
    ok(!open(my $fh, '<:async', $file), 'open of an unreadable file fails');
    is($! + 0, EACCES, '$! is EACCES for an unreadable file');
    chmod 0644, $file;
}

# ---- MT::IO: stat, splice, fsync, aio_stats --------------------------------------

{
    spew(':unix', $file, $text);
    my @mine = MT::IO::stat($file);
    my @theirs = stat $file;
    is(scalar(@mine), 13, 'MT::IO::stat returns thirteen values');
    is(join(',', @mine[0 .. 4, 6 .. 7]), join(',', @theirs[0 .. 4, 6 .. 7]),
              'MT::IO::stat agrees with stat on dev, ino, mode, nlink, uid, rdev, size');
    my @none = MT::IO::stat('/nonexistent/perl5-mt/file');
    is(scalar(@none), 0, 'MT::IO::stat of a missing path is the empty list');
}

{
    my $dst = tempfile();
    spew(':unix', $file, $text);
    open my $in, '<:unix', $file or die "open: $!";
    open my $out, '>:unix', $dst or die "open: $!";
    my $moved = MT::IO::splice($in, $out, length $text);
    close $out;
    close $in;
    is($moved, length $text, 'MT::IO::splice moves the whole file');
    is(slurp(':unix', $dst), $text, 'MT::IO::splice copies byte for byte');
    unlink $dst;
}

{
    open my $fh, '>:async', $file or die "open: $!";
    print $fh "to disk\n";
    ok(MT::IO::fsync($fh), 'MT::IO::fsync is true on a written handle');
    close $fh;
}

{
    my %s = MT::IO::aio_stats();
    ok(exists $s{submitted} && exists $s{completed} && exists $s{buffers},
       'MT::IO::aio_stats reports the ring counters');
}

# ---- fibers: many reads in flight, one scheduler -------------------------------------

{
    my $out = child(<<'CODE');
use strict; use warnings;
my $dir = $ENV{TMPDIR} || '/tmp';
my @files;
for my $i (1 .. 64) {
    my $f = "$dir/perlio_async_fib_$$" . "_$i";
    open my $w, '>:unix', $f or die "open $f: $!";
    print $w chr(65 + $i % 26) x (1024 * 1024);
    close $w;
    push @files, $f;
}
my $max = 0;
my $peak = 0;
my @ids = map {
    my $f = $_;
    MT::Fiber::schedule(sub {
        my $s = { MT::IO::aio_stats() };
        $peak = $s->{pending} if $s->{pending} > $peak;
        open my $fh, '<:async', $f or die "open $f: $!";
        binmode $fh;
        local $/;
        my $data = <$fh>;
        close $fh;
        $data;
    });
} @files;
MT::Fiber::run();
my $bad = 0;
for my $i (0 .. $#ids) {
    my $d = MT::Fiber::join($ids[$i]);
    $bad++ unless length($d) == 1024 * 1024 && substr($d, 0, 1) eq chr(65 + ($i + 1) % 26);
}
my %s = MT::IO::aio_stats();
print "bad=$bad,parked=$s{parked},peak=$peak\n";
unlink @files;
CODE
    like($out, qr/^bad=0,parked=\d+,peak=\d+$/m, '64 fibers each read their own 1 MiB file through :async');
    my ($parked) = $out =~ /parked=(\d+)/;
    ok($parked > 0, 'the fibers parked on their reads and the loop woke them');
    my ($peak) = $out =~ /peak=(\d+)/;
    # a page-cache read completes at its submit, so the ring rarely holds
    # two of these at once: the peak is a note, not a claim
    print "# 64 fibers: most reads in flight at one submit: $peak\n";
}

# ---- the ring in workers -----------------------------------------------------------------

{
    my $out = child(<<'CODE');
use strict; use warnings;
my $f = ($ENV{TMPDIR} || '/tmp') . "/perlio_async_w_$$";
open my $w, '>:unix', $f or die "open: $!";
print $w "worker data\n" x 1000;
close $w;
sub work {
    open my $fh, '<:async', $f or die "open: $!";
    local $/;
    my $d = <$fh>;
    close $fh;
    return length($d);
}
MT::Image::freeze();
my @ids = map { MT::Worker::spawn('main::work') } 1 .. 16;
my %seen;
$seen{ MT::Worker::join($_) }++ for @ids;
print join(" ", map { "$_=$seen{$_}" } sort keys %seen), "\n";
print "one=", MT::Worker::join(MT::Worker::spawn('main::work')), "\n";
unlink $f;
CODE
    like($out, qr/^12000=16$/m, 'sixteen workers read the same file through their own rings');
    like($out, qr/^one=12000$/m, 'one worker reads through its ring');
}

# ---- a read in flight while a remote XS call is parked ------------------------------

SKIP: {
    my $probe = child(q{require MT::XSTest::Legacy; print "ok\n"});
    skip("MT::XSTest::Legacy is not built (make -C ext/MT-XSTest-Legacy)", 2)
        unless $probe =~ /^ok$/m;
    my $out = child(<<'CODE');
use strict; use warnings;
my $f = ($ENV{TMPDIR} || '/tmp') . "/perlio_async_xs_$$";
open my $w, '>:unix', $f or die "open: $!";
print $w "xs and aio\n" x 100000;
close $w;
sub work {
    require MT::XSTest::Legacy;
    my $read = MT::Fiber::schedule(sub {
        open my $fh, '<:async', $f or die "open: $!";
        local $/;
        my $d = <$fh>;
        close $fh;
        length $d;
    });
    my $calls = MT::Fiber::schedule(sub {
        my $bad = 0;
        for my $i (1 .. 200) {
            $bad++ unless MT::XSTest::Legacy::sum(7, $i) == 7 + $i;
        }
        $bad;
    });
    MT::Fiber::run();
    return "read=" . MT::Fiber::join($read) . ",bad=" . MT::Fiber::join($calls);
}
MT::Image::freeze();
print "xs: ", MT::Worker::join(MT::Worker::spawn('main::work')), "\n";
unlink $f;
CODE
    like($out, qr/^xs: read=1100000,bad=0$/m, 'a read in flight and a parked remote call both complete in one loop');
    like($out, qr/^xs: /m, 'the fibers of the XS case return');
}

# ---- the main context without a scheduler loop -----------------------------------------

{
    my $c = child(<<'CODE');
open my $fh, '<:async', $0 or die "open: $!";
my $n = 0; $n++ while <$fh>;
close $fh;
print "lines=", ($n > 0 ? "yes" : "no"), "\n";
CODE
    like($c, qr/^lines=yes$/m, 'the layer reads in the main context with no scheduler loop');
}

unlink $file;

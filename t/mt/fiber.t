#!./perl
# perl5-mt: fibers (mt_fiber.c).  Asymmetric coroutines on guarded
# stacklets: resume and yield, the interpreter state they swap (lexicals,
# local, @_, the mark and scope stacks, wantarray, eval, sort and map, the
# match state), the error paths, and fibers inside workers.  Every case runs
# in a child perl, as freeze.t does: a frozen image is process-wide.

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib');
}

use strict;
use warnings;

plan(tests => 24);

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

# ---- resume and yield carry values both ways -------------------------------

{
    my $out = child(<<'CODE');
my $f = MT::Fiber::spawn(sub {
    my $y = MT::Fiber::yield($_[0] * 2);
    my $z = MT::Fiber::yield($y + 1);
    "done:$z";
}, 5);
my @r = (MT::Fiber::resume($f), MT::Fiber::resume($f, 7), MT::Fiber::resume($f, 'z'));
print join(",", @r), "\n";
print MT::Fiber::is_finished($f) ? "finished\n" : "running\n";
print MT::Fiber::join($f), "\n";
print MT::Fiber::count(), "\n";
CODE
    is($out, "10,8,done:z\nfinished\ndone:z\n0\n",
       'resume and yield pass values both ways, join returns the result');
}

# ---- the interpreter state survives a yield inside nested calls ------------

{
    my $out = child(<<'CODE');
our $g = 'global';
sub inner {
    my ($n) = @_;
    my $lex = "lex$n";
    local $g = "loc$n";
    my @got = (1, 2, MT::Fiber::yield($lex, $g, scalar(@_), wantarray ? 'L' : 'S'), 3);
    return "$lex/$got[2]/$g/" . scalar(@_) . '/' . (wantarray ? 'L' : 'S')
           . '/' . join(',', @got);
}
sub outer { my @r = inner($_[0]); return "outer($_[0]):" . join(';', @r) }
my $f = MT::Fiber::spawn(sub { my @x = (outer('a'), 'after'); return join('|', @x) . "|$g" });
print join(' ', MT::Fiber::resume($f)), "\n";
print MT::Fiber::resume($f, 'back'), "\n";
CODE
    is($out,
       "lexa loca 1 L\n"
       . "outer(a):lexa/back/loca/1/L/1,2,back,3|after|global\n",
       'lexicals, local, @_, wantarray and the mark stack across a yield');
}

# ---- yield inside sort, map and eval ----------------------------------------

{
    my $out = child(<<'CODE');
my $f = MT::Fiber::spawn(sub {
    my @s = sort { MT::Fiber::yield("cmp"); $a <=> $b } (3, 1, 2);
    my @m = map { MT::Fiber::yield("map$_"); $_ * 10 } (1, 2);
    my $ok = eval { MT::Fiber::yield("eval"); die "after yield\n"; 1 };
    my $err = $@;
    return "@s|@m|" . ($ok ? 'ok' : 'failed') . "|$err";
});
my @seen;
while (1) {
    my @v = MT::Fiber::resume($f);
    last if MT::Fiber::is_finished($f);
    push @seen, $v[0];
}
print join(',', grep { $_ ne 'cmp' } @seen), "\n";
print scalar(grep { $_ eq 'cmp' } @seen) >= 2 ? "cmp ok\n" : "cmp few\n";
print MT::Fiber::join($f), "\n";
CODE
    like($out, qr/^map1,map2,eval$/m, 'yield inside a map block and inside eval');
    like($out, qr/^cmp ok$/m,         'yield inside a sort comparator');
    like($out, qr/^1 2 3\|10 20\|failed\|after yield$/m,
         'a die after the yield is caught by the eval that the fiber entered');
}

# ---- die inside a fiber, rethrown by join ----------------------------------

{
    my $out = child(<<'CODE');
my $f = MT::Fiber::spawn(sub { die "plain\n" });
eval { MT::Fiber::join($f) }; print "1: $@";
my $g = MT::Fiber::spawn(sub { MT::Fiber::yield(1); die bless({ code => 42 }, 'Err') });
MT::Fiber::resume($g);
eval { MT::Fiber::join($g) }; print "2: ", ref($@), " ", $@->{code}, "\n";
print "3: ", MT::Fiber::count(), "\n";
CODE
    is($out, "1: plain\n2: Err 42\n3: 0\n",
       'a die in a fiber is rethrown by join, an object intact');
}

# ---- exit inside a fiber ---------------------------------------------------

{
    my $out = child(<<'CODE');
my $f = MT::Fiber::spawn(sub { exit 3 });
eval { MT::Fiber::join($f) };
print "exit: $@";
print "after: ", MT::Fiber::count(), "\n";
CODE
    like($out, qr/^exit: perl5-mt: exit\(\) called inside a fiber \(MT_EXIT_IN_FIBER\)/m,
         'exit inside a fiber is MT_EXIT_IN_FIBER at join');
    like($out, qr/^after: 0$/m, '... and the process goes on');
}

# ---- the error paths -------------------------------------------------------

{
    my $out = child(<<'CODE');
eval { MT::Fiber::yield(1) }; print "main: $@";
my $f;
$f = MT::Fiber::spawn(sub { eval { MT::Fiber::resume($f) }; print "self: $@"; 'x' });
print "joined: ", MT::Fiber::join($f), "\n";
eval { MT::Fiber::resume($f) }; print "gone: $@";
my $g = MT::Fiber::spawn(sub { 1 });
MT::Fiber::resume($g);
eval { MT::Fiber::resume($g) }; print "done: $@";
CODE
    like($out, qr/^main: perl5-mt: operation is only valid inside a fiber \(MT_NOT_IN_FIBER\)/m,
         'yield from the main context is MT_NOT_IN_FIBER');
    like($out, qr/^self: perl5-mt: fiber is running.*\(MT_FIBER_BUSY\)/m,
         'resume of the running fiber is MT_FIBER_BUSY');
    like($out, qr/^gone: perl5-mt: no such fiber .*\(MT_NOT_A_FIBER\)/m,
         'an id that was joined is MT_NOT_A_FIBER');
    like($out, qr/^done: perl5-mt: fiber has already finished \(MT_FIBER_FINISHED\)/m,
         'resume of a finished fiber is MT_FIBER_FINISHED');
}

# ---- a fiber resuming another fiber, two deep -------------------------------

{
    my $out = child(<<'CODE');
my $inner;
my $outer = MT::Fiber::spawn(sub {
    $inner = MT::Fiber::spawn(sub {
        my $v = MT::Fiber::yield('i1');
        MT::Fiber::yield("i2:$v");
        'idone';
    });
    my $a = MT::Fiber::resume($inner);
    my $b = MT::Fiber::yield("o1:$a");
    my $c = MT::Fiber::resume($inner, $b);
    MT::Fiber::yield("o2:$c");
    MT::Fiber::resume($inner, 'last');
    'outer done';
});
my $r1 = MT::Fiber::resume($outer);
my $r2 = MT::Fiber::resume($outer, 'B');
my $r3 = MT::Fiber::resume($outer, 'C');
print "$r1 | $r2 | $r3\n";
print MT::Fiber::join($inner), " ", MT::Fiber::count(), "\n";
CODE
    is($out, "o1:i1 | o2:i2:B | outer done\nidone 1\n",
       'a fiber resumes a fiber, both yield, values go to the right resumer');
}

# ---- many fibers, one stacklet reused --------------------------------------

{
    my $n = $long ? 100_000 : 10_000;
    my $code = <<'CODE';
my $n = NN;
my $sum = 0;
for my $i (1 .. $n) {
    my $id = MT::Fiber::spawn(sub { MT::Fiber::yield($_[0]); $_[0] + 1 }, $i);
    my ($y) = MT::Fiber::resume($id);
    $sum += $y + MT::Fiber::join($id);
}
print "sum=$sum count=", MT::Fiber::count(), "\n";
my $s = MT::Fiber::stats();
print join(" ", map { "$_=$s->{$_}" } sort keys %$s), "\n";
CODE
    $code =~ s/\bNN\b/$n/;
    my $out = child($code);
    my $want = $n * $n + 2 * $n;
    like($out, qr/^sum=\Q$want\E count=0$/m,
         "$n fibers spawned, resumed once and joined");
    like($out, qr/^free=1 mapped=1 mmaps=1 peak=1$/m,
         '... on one stacklet, the free list reused LIFO');
}

# ---- the match state of two fibers, interleaved by yields ------------------

{
    my $out = child(<<'CODE');
my ($s1, $s2) = ("alpha beta gamma", "one two three");
my $f1 = MT::Fiber::spawn(sub {
    while ($s1 =~ /(\w+)/g) {
        my ($w, $p) = ($1, pos($s1));
        MT::Fiber::yield("f1:$w:$p");
        die "f1 lost its match\n" unless $1 eq $w and pos($s1) == $p;
    }
    'f1done';
});
my $f2 = MT::Fiber::spawn(sub {
    while ($s2 =~ /(\w+)/g) {
        my ($w, $p) = ($1, pos($s2));
        MT::Fiber::yield("f2:$w:$p");
        die "f2 lost its match\n" unless $1 eq $w and pos($s2) == $p;
    }
    'f2done';
});
my @seen;
for (1 .. 4) {
    push @seen, MT::Fiber::resume($f1), MT::Fiber::resume($f2);
}
print join(' ', @seen), "\n";
CODE
    is($out,
       "f1:alpha:5 f2:one:3 f1:beta:10 f2:two:7 f1:gamma:16 f2:three:13 f1done f2done\n",
       'pos and $1 of two fibers interleaved by yields stay their own');
}

# ---- fibers inside workers -------------------------------------------------

my $worker_f = <<'CODE';
sub f {
    my $fid = MT::Fiber::spawn(sub { my $x = MT::Fiber::yield($_[0] + 1); "got$x" }, 41);
    my $y = MT::Fiber::resume($fid);
    my @z = MT::Fiber::resume($fid, 'v');
    my $fin = MT::Fiber::join($fid);
    return "w$y|$z[0]|$fin|" . MT::Fiber::count();
}
CODE

{
    my $out = child($worker_f . <<'CODE');
MT::Image::freeze();
print MT::Worker::join(MT::Worker::spawn('main::f')), "\n";
CODE
    is($out, "w42|gotv|gotv|0\n", 'resume and yield inside a worker');
}

{
    my $out = child($worker_f . <<'CODE');
MT::Image::freeze();
my @w = map { MT::Worker::spawn('main::f') } 1 .. 16;
my %r;
$r{ MT::Worker::join($_) }++ for @w;
print join(",", map { "$_ x$r{$_}" } sort keys %r), "\n";
CODE
    is($out, "w42|gotv|gotv|0 x16\n", 'sixteen workers resume and yield at once');
}

{
    my $out = child(<<'CODE');
MT::Image::freeze();
my $id = MT::Fiber::spawn(sub { 1 });
sub g { my $r = eval { MT::Fiber::resume($_[0]); 1 } ? 'resumed' : $@; $r }
print MT::Worker::join(MT::Worker::spawn('main::g', $id)), "\n";
CODE
    like($out, qr/MT_NOT_A_FIBER/, 'an id from another worker names no fiber there');
}

# ---- yield during compilation ----------------------------------------------

{
    my $out = child(<<'CODE');
my $f = MT::Fiber::spawn(sub {
    eval q{ BEGIN { MT::Fiber::yield('in begin') } 1 } or return "failed: $@";
    'compiled';
});
print MT::Fiber::join($f), "\n";
CODE
    like($out, qr/MT_YIELD_IN_COMPILE/, 'yield inside BEGIN in a fiber is MT_YIELD_IN_COMPILE');
}

# ---- concurrent reentrancy into the same sub (non-LIFO exit, recursion, :async)

{
    my $out = child(<<'CODE');
sub shared_sub {
    my ($tag, $val) = @_;
    my $lex = "$tag:$val";
    MT::Fiber::yield("mid:$lex:$_[0]");
    return "end:$lex:$_[0]:$_[1]";
}
my $f1 = MT::Fiber::spawn(\&shared_sub, 'f1', 10);
my $f2 = MT::Fiber::spawn(\&shared_sub, 'f2', 20);
my $f3 = MT::Fiber::spawn(\&shared_sub, 'f3', 30);
# Interleave non-LIFO: f1 enters, f2 enters, f1 exits, f3 enters (must not collide with f2),
# then f2 exits, then f3 exits.
my @r;
push @r, MT::Fiber::resume($f1);
push @r, MT::Fiber::resume($f2);
push @r, MT::Fiber::resume($f1);
push @r, MT::Fiber::resume($f3);
push @r, MT::Fiber::resume($f2);
push @r, MT::Fiber::resume($f3);
print join("\n", @r), "\n";
CODE
    is($out,
       "mid:f1:10:f1\nmid:f2:20:f2\nend:f1:10:f1:10\nmid:f3:30:f3\nend:f2:20:f2:20\nend:f3:30:f3:30\n",
       'concurrent fibers entering the same sub non-LIFO keep pads and @_ isolated');
}

{
    my $out = child(<<'CODE');
sub fact {
    my ($tag, $n) = @_;
    my $cur = "$tag($n)";
    MT::Fiber::yield($cur);
    return 1 if $n <= 1;
    my $sub = fact($tag, $n - 1);
    die "corrupt lex $cur vs $tag($n)" unless $cur eq "$tag($n)" && $_[0] eq $tag && $_[1] == $n;
    return $n * $sub;
}
my $f1 = MT::Fiber::spawn(\&fact, 'A', 4);
my $f2 = MT::Fiber::spawn(\&fact, 'B', 3);
my $f3 = MT::Fiber::spawn(\&fact, 'C', 5);
my @live = ($f1, $f2, $f3);
while (@live) {
    my @next;
    for my $f (@live) {
        MT::Fiber::resume($f);
        push @next, $f unless MT::Fiber::is_finished($f);
    }
    @live = @next;
}
print join(",", MT::Fiber::join($f1), MT::Fiber::join($f2), MT::Fiber::join($f3)), "\n";
CODE
    is($out, "24,6,120\n",
       'concurrent fibers recursing into the same sub keep pads and @_ isolated');
}

{
    my $file = tempfile();
    open my $w, '>:unix', $file or die "open $file: $!";
    print $w "z" x 8192 or die "write: $!";
    close $w or die "close: $!";
    my $code = <<'CODE';
my $path = q{PATH_PLACEHOLDER};
sub read_async {
    my ($id) = @_;
    open my $fh, '<:async', $path or die "open $path: $!";
    local $/;
    my $data = <$fh>;
    close $fh or die "close: $!";
    die "bad len for $id" unless defined $data && length($data) == 8192;
    return $id;
}
my @fids;
for my $f (1 .. 6) {
    push @fids, MT::Fiber::schedule(sub {
        my $sum = 0;
        for my $i (1 .. 25) {
            $sum += read_async($f * 100 + $i);
            MT::Fiber::pass() if ($i & 1) == 0;
        }
        return $sum;
    });
}
MT::Fiber::run();
my $total = 0;
$total += MT::Fiber::join($_) for @fids;
print "total=$total\n";
CODE
    $code =~ s/PATH_PLACEHOLDER/$file/;
    my $out = child($code);
    # sum_{f=1..6} sum_{i=1..25} (f*100 + i) = 6*25*(350) + 6*325 = 52500 + 1950 = 54450
    is($out, "total=54450\n",
       'scheduled fibers concurrently entering an :async sub do not collide on pad slots');
}

# ---- the cost of a round trip, for the record ------------------------------

{
    my $n = $long ? 1_000_000 : 100_000;
    my $code = <<'CODE';
require Time::HiRes;
my $f = MT::Fiber::spawn(sub { while (1) { MT::Fiber::yield(1) } });
my $t0 = Time::HiRes::time();
MT::Fiber::resume($f) for 1 .. NN;
my $dt = Time::HiRes::time() - $t0;
printf "%.1f\n", $dt / NN * 1e9;
CODE
    $code =~ s/\bNN\b/$n/g;
    my $out = child($code);
    my ($ns) = $out =~ /^(\d+\.\d)$/m;
    ok(defined $ns, 'resume and yield round trips ran');
    note("resume+yield round trip: $ns ns ($n round trips in one fiber)")
        if defined $ns;
}

#!./perl
# perl5-mt: the sealed boot image (perl_freeze_boot_image) and
# worker interpreters.  Every case runs in a child perl: a frozen image is
# process-wide and permanent.

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib');
}

use strict;
use warnings;
use Config;

plan(tests => 26);

my $linux = $^O eq 'linux';

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

# ---- an explicit freeze at runtime -----------------------------------------

{
    my $out = child(<<'CODE');
MT::Image::freeze();
my $s = MT::Image::stats();
print join(" ", map { "$_=$s->{$_}" } sort keys %$s), "\n";
CODE
    like($out, qr/\bfrozen=1\b/,  'MT::Image::freeze freezes');
    like($out, qr/\bsealed=1\b/,  '... and seals the arena');
    like($out, qr/\barena_used=[1-9]\d{4,}\b/, '... with tens of kB used');
    like($out, qr/\barena_overflow=0\b/, '... and nothing spilt to the heap');
    like($out, qr/\bn_sv=[1-9]\d{2,}\b/, '... hundreds of SVs');
    like($out, qr/\bn_cv=[1-9]\d+\b/,    '... dozens of CVs');
    like($out, qr/\bn_gp=[1-9]\d+\b/,    '... dozens of GPs');
    like($out, qr/\bn_hv=[1-9]\d*\b/,    '... and hashes, all with an aux');
    like($out, qr/\bops_in_arena=0\b/,   'a runtime freeze leaves the ops on the heap');
  SKIP: {
        skip 'soft-dirty tracking is Linux only', 1 unless $linux;
        like($out, qr/\bwritten_kb=0\b/, 'no page of the image written after sealing');
    }
}

# ---- the parent keeps working on its own copy ------------------------------

{
    my $out = child(<<'CODE');
sub fact { my ($n) = @_; $n <= 1 ? 1 : $n * fact($n - 1) }
my %h = (a => 1);
MT::Image::freeze();
my $x = 1; $x++;
my @s = sort { $a <=> $b } (3, 1, 2);
$h{b} = 2;
my $re = qr/(\d+)/;
"abc123" =~ $re or die;
eval "sub later { 43 } 1" or die $@;
require List::Util;
print join(" ", $x, "@s", join(",", map { "$_=$h{$_}" } sort keys %h), $1,
           later(), List::Util::sum(1 .. 4), fact(5)), "\n";
eval { MT::Image::freeze() };
print "again: $@";
CODE
    like($out, qr/^2 1 2 3 a=1,b=2 123 43 10 120$/m,
         'lexicals, sort, hashes, regexps, eval, require and recursion after the freeze');
    like($out, qr/MT_ALREADY_FROZEN/, 'a second freeze dies with MT_ALREADY_FROZEN');
}

# ---- PERL_MT_IMAGE=1: ops in the arena, automatic freeze after INIT --------

{
    local $ENV{PERL_MT_IMAGE} = 1;
    my $out = child(<<'CODE');
our $phase = 'compile';
BEGIN { $main::seen_begin = MT::Image::frozen() ? 'frozen' : 'not yet' }
INIT  { $main::seen_init  = MT::Image::frozen() ? 'frozen' : 'not yet' }
my $s = MT::Image::stats();
print "begin=$main::seen_begin init=$main::seen_init run=", (MT::Image::frozen() ? 'frozen' : 'no'), "\n";
print "ops_in_arena=$s->{ops_in_arena} used=$s->{arena_used} written=$s->{written_kb}\n";
sub fib { my $n = shift; $n < 2 ? $n : fib($n-1) + fib($n-2) }
my @l = map { fib($_) } 0 .. 10;
my $str = join ",", @l;
$str =~ s/,/ /g;
eval "sub dyn { 'dyn ok' } 1" or die $@;
require Carp;
print "$str ", dyn(), " ", (eval { Carp::croak('c') } ? 'no' : 'croaked'), "\n";
for my $i (1 .. 3) { last if $i == 2 }
print "loops ok\n";
CODE
    like($out, qr/^begin=not yet init=not yet run=frozen$/m,
         'PERL_MT_IMAGE=1 freezes after INIT, before the main program');
    like($out, qr/^ops_in_arena=1 /m, '... with the op slabs in the arena');
    like($out, qr/^0 1 1 2 3 5 8 13 21 34 55 dyn ok croaked$/m,
         '... and the program runs on the sealed ops');
    like($out, qr/^loops ok$/m, '... loops included');
}

# ---- the image is bigger than the program's own data ------------------------

{
    my $out = child(<<'CODE');
my @big = map { "x" x 100 } 1 .. 1000;
MT::Image::freeze();
my $s = MT::Image::stats();
print "used=$s->{arena_used}\n";
CODE
    my ($used) = $out =~ /used=(\d+)/;
    ok($used && $used > 100_000, "the image carries the program's data ($used bytes)");
}

# ---- stats before any freeze ----------------------------------------------

{
    my $out = child('my $s = MT::Image::stats(); print "frozen=$s->{frozen} ", MT::Image::frozen() ? 1 : 0, "\n"');
    like($out, qr/^frozen=0 0$/m, 'stats and frozen() before a freeze');
}

# ---- workers ---------------------------------------------------------------

SKIP: {
    skip 'MT::Worker not available yet', 8 unless defined &MT::Worker::spawn;
    my $out = child(<<'CODE');
our $counter = 10;
sub work { my ($id, $n) = @_; my $sum = 0; $sum += $_ for 1 .. $n; $counter++; return "w$id:$sum:$counter" }
MT::Image::freeze();
my @ids = map { MT::Worker::spawn('main::work', $_, 1000) } 1 .. 16;
my @res = map { MT::Worker::join($_) } @ids;
print join(" ", sort @res), "\n";
print "parent counter=$counter\n";
my $s = MT::Image::stats();
print "written=$s->{written_kb}\n";
CODE
    like($out, qr/w1:500500:11/, 'a worker ran the frozen sub');
    like($out, qr/w16:500500:11/, '... all sixteen of them');
    like($out, qr/parent counter=10/, 'our variables are worker-private');
  SKIP: {
        skip 'soft-dirty tracking is Linux only', 1 unless $linux;
        like($out, qr/^written=0$/m, 'no page of the image written by the workers');
    }
    my $err = child(<<'CODE');
MT::Image::freeze();
my $id = MT::Worker::spawn('main::nosuch');
my $r = eval { MT::Worker::join($id) };
print "join: ", defined $r ? $r : "undef", " err=$@";
CODE
    like($err, qr/err=.*MT_WORKER_FAILED/, 'a missing entry sub fails at join');
    my $die = child(<<'CODE');
sub boom { die "worker died\n" }
MT::Image::freeze();
my $id = MT::Worker::spawn('main::boom');
my $r = eval { MT::Worker::join($id) };
print "join: ", defined $r ? $r : "undef", " err=$@";
CODE
    like($die, qr/err=worker died/, 'a dying worker surfaces its error at join');
    my $w = child(<<'CODE');
use Scalar::Util qw(weaken);
sub wk { my $h = { a => [1, 2, 3] }; my $r = $h; weaken($r); my $alive = defined $r ? 1 : 0; undef $h; my $gone = defined $r ? 0 : 1; eval "sub inner { 7 } 1" or die $@; require Text::Wrap; return "$alive$gone" . inner() . length(Text::Wrap::wrap('', '', 'a b')) }
MT::Image::freeze();
print MT::Worker::join(MT::Worker::spawn('main::wk')), "\n";
CODE
    like($w, qr/^1173$/m, 'weaken, runtime eval and require work inside a worker');
    my $many = child(<<'CODE');
sub churn { my $n = 0; for my $i (1 .. 100_000) { my %h = (k => $i); my @a = (1 .. 3); $n += $h{k} % 7 + @a } return $n }
MT::Image::freeze();
my @ids = map { MT::Worker::spawn('main::churn') } 1 .. 16;
my %r; $r{ MT::Worker::join($_) }++ for @ids;
print join(",", map { "$_=$r{$_}" } keys %r), "\n";
CODE
    like($many, qr/^\d+=16$/m, '16 workers times 100k ops agree');
}

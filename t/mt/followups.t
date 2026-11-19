#!./perl
# perl5-mt: worker regression tests: an XS module with
# MY_CXT but no CLONE is refused by the freeze, overloads defined inside a
# worker on an image class are seen, and closures created inside a worker
# work (their pad name list is the image's, never reference counted).

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib');
}

use strict;
use warnings;

plan(tests => 9);

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

# ---- MY_CXT without CLONE -------------------------------------------------

{
    my $out = child(<<'CODE');
require MT::XSTest::Legacy;          # MY_CXT, no CLONE, loaded into the image
eval { MT::Image::freeze() };
print "freeze: $@";
CODE
    like($out, qr/^freeze: perl5-mt: XS module MT::XSTest::Legacy uses MY_CXT without a CLONE method \(MT_MY_CXT_WITHOUT_CLONE\)/m,
         'the freeze refuses an image that contains an XS module with MY_CXT but no CLONE, naming it');
}

{
    my $out = child(<<'CODE');
require MT::XSTest::Legacy;
$MT::XS::inprocess_ok{'MT::XSTest::Legacy'} = 1;
require MT::XSTest;                  # MY_CXT with CLONE: always fine
MT::Image::freeze();
sub work { return MT::XSTest::bump() . "," . MT::XSTest::Legacy::sum(1, 2) }
print "frozen: ", MT::Worker::join(MT::Worker::spawn('main::work')), "\n";
CODE
    like($out, qr/^frozen: 1,3$/m,
         'a module vouched for in %MT::XS::inprocess_ok, and one with CLONE, freeze fine');
}

# ---- overloads defined inside a worker on an image class --------------------

{
    my $out = child(<<'CODE');
package Img;                          # in the image, no overloads at the freeze
sub new { my ($class, $v) = @_; return bless { v => $v }, $class }
package Half;                         # in the image with one overload
use overload '+' => sub { Half->new($_[0]{v} + $_[1]{v}) };
sub new { my ($class, $v) = @_; return bless { v => $v }, $class }
package main;
sub work {
    my @r;
    eval q{
        package Img;
        use overload '+' => sub { Img->new($_[0]{v} + $_[1]{v}) },
                     '""' => sub { "Img(" . $_[0]{v} . ")" };
        package Half;
        use overload '-' => sub { Half->new($_[0]{v} - $_[1]{v}) },
                     '""' => sub { "Half(" . $_[0]{v} . ")" };
        1;
    } or return "eval: $@";
    my $s = Img->new(2) + Img->new(3);
    push @r, "$s";
    push @r, "" . (Half->new(9) - Half->new(4));
    push @r, "" . (Half->new(1) + Half->new(1));
    eval { no overload '+'; 1 };
    return join ",", @r;
}
MT::Image::freeze();
print "worker: ", MT::Worker::join(MT::Worker::spawn('main::work')), "\n";
print "worker2: ", MT::Worker::join(MT::Worker::spawn('main::work')), "\n";
my $m = Img->new(1);
print "main: ", ("$m" =~ /^Img=HASH/ ? "unaffected" : "$m"), "\n";
CODE
    like($out, qr/^worker: Img\(5\),Half\(5\),Half\(2\)$/m,
         'overloads defined inside a worker on image classes are used, old ones kept');
    like($out, qr/^worker2: Img\(5\),Half\(5\),Half\(2\)$/m, '... in every worker that defines them');
    like($out, qr/^main: unaffected$/m, '... and the main interpreter does not see them');
}

# ---- closures created inside a worker ------------------------------------------

{
    my $out = child(<<'CODE');
sub make_counter { my $n = shift; return sub { return $n++ } }   # in the image
sub work {
    my ($id) = @_;
    my @r;
    my $acc = 0;
    my $add = sub { $acc += $_[0] };                 # captures a lexical of an image sub
    $add->($_) for 1 .. 10;
    push @r, $acc;
    my $c = make_counter(5);                        # a closure cloned from an image closure
    $c->(); $c->();
    push @r, $c->();
    my $outer = "o${id}_";
    my $nested = sub { my $x = shift; return sub { "$outer:$x:$_[0]" } };
    push @r, $nested->("m")->("i");
    my $ev = eval q{ my $y = 2; sub { $outer . $y . $_[0] } } or return "eval: $@";
    push @r, $ev->("!");
    my @subs = map { my $k = $_; sub { $k * 2 } } 1 .. 1000;    # many closures
    my $sum = 0; $sum += $_->() for @subs;
    push @r, $sum;
    undef @subs;                                    # and their release
    return join ",", @r;
}
MT::Image::freeze();
my @ids = map { MT::Worker::spawn('main::work', $_) } 1 .. 16;
my %seen; for my $id (@ids) { my $r = MT::Worker::join($id); $r =~ s/o\d+_/oN_/g; $seen{$r}++ }
print join(" ", map { "$_=$seen{$_}" } sort keys %seen), "\n";
my $c = make_counter(1); $c->();
print "main: ", $c->(), "\n";
CODE
    like($out, qr/^55,7,oN_:m:i,oN_2!,1001000=16$/m,
         'sixteen workers create closures over their lexicals, clone image closures, nest them and build them in string eval');
    like($out, qr/^main: 2$/m, 'the main interpreter is unaffected');
}

# ---- closures passed to a legacy XSUB in a compartment --------------------------

{
    my $out = child(<<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my $n = 0;
    MT::XSTest::Legacy::callback(sub { $n += $_[0] }, 10) for 1 .. 50;
    my @many = map { my $k = $_; MT::XSTest::Legacy::callback_scalar(sub { $k + $_[0] }, 1) } 1 .. 200;
    return "$n," . ($many[0] + $many[199]);
}
MT::Image::freeze();
print "cb: ", MT::Worker::join(MT::Worker::spawn('main::work')), "\n";
print "stats: ", MT::XS::bridge_stats()->{callbacks}, "\n";
CODE
    like($out, qr/^cb: 500,203$/m, 'a closure created per call is pinned, called back and released');
    like($out, qr/^stats: \d+$/m, 'bridge stats are readable from the main interpreter');
}

#!./perl
# perl5-mt: the two-tier XS dispatcher, exercised with the two
# fixture modules (ext/MT-XSTest, threadsafe; ext/MT-XSTest-Legacy, served by
# an XS compartment).  Every case runs in a child perl: a frozen image and
# its compartments are process-wide and permanent.

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib');
}

use strict;
use warnings;
use Config;

plan(tests => 21);

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

my $prelude = <<'PRELUDE';
$| = 1;
sub say_ { print @_, "\n" }
sub run_worker { my ($sub, @args) = @_; return MT::Worker::join(MT::Worker::spawn($sub, @args)) }
sub err { my ($e) = @_; $e =~ s/ at \S+ line \d+\.?\n?//; chomp $e; return $e }
PRELUDE

# ---- tier one: a PERL_XS_THREADSAFE module in sixteen workers -------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest;
    my @r;
    push @r, MT::XS::is_threadsafe(\&MT::XSTest::add) ? "safe" : "unsafe";
    push @r, MT::XS::is_remote(\&MT::XSTest::add) ? "remote" : "local";
    push @r, MT::XSTest::add(2, 3);
    push @r, MT::XSTest::bump(), MT::XSTest::bump();    # a fresh MY_CXT per worker
    push @r, MT::XSTest::worker_id() == $_[0] ? "myid" : "otherid";
    return join ",", @r;
}
MT::Image::freeze();
my @ids = map { MT::Worker::spawn('main::work', 0) } 1 .. 16;
my %seen; $seen{ MT::Worker::join($_) }++ for @ids;
say_ join " ", map { "$_=$seen{$_}" } sort keys %seen;
CODE
    like($out, qr/^safe,local,5,1,2,otherid=16$/m,
         'sixteen workers load a threadsafe module in-process, each with its own MY_CXT');
}

# ---- tier two: the legacy module goes to a compartment ---------------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my @r;
    push @r, MT::XS::is_remote(\&MT::XSTest::Legacy::sum) ? "remote" : "local";
    push @r, MT::XS::is_threadsafe(\&MT::XSTest::Legacy::sum) ? "safe" : "unsafe";
    push @r, MT::XSTest::Legacy::sum(1, 2, 3);
    push @r, MT::XSTest::Legacy::pid() == $$ ? "samepid" : "otherpid";
    push @r, MT::XSTest::Legacy::call_perl_side("q");
    push @r, MT::XS::stats()->{compartment};
    return join ",", @r;
}
MT::Image::freeze();
say_ "first: ", run_worker('main::work');
say_ "second: ", run_worker('main::work');
say_ "main: ", (MT::XS::is_remote(\&MT::XSTest::Legacy::sum) ? "remote" : "none");
CODE
    like($out, qr/^first: remote,unsafe,6,otherpid,perl_side\(q\),\d$/m,
         'a module without the marker is served by a compartment: remote stub, other process, its .pm half is there');
    like($out, qr/^second: remote,unsafe,6,otherpid,perl_side\(q\),\d$/m,
         'a second worker loading the same module gets the stubs again');
    like($out, qr/^main: none$/m, 'the main interpreter never saw the module');
}

# ---- marshalling: every TLV type round-trips --------------------------------

{
    my $out = child($prelude . <<'CODE');
use utf8;
sub work {
    require MT::XSTest::Legacy;
    my @in = (undef, 0, 1, -5, 2**40, 18446744073709551615, 2.5, -0.125,
              "", "plain", "caf\x{e9}", "\x{263a} smiley", "123", "1e3",
              !!1, !!0, [1, [2, 3], { a => 1 }], { k => "v", n => [1, 2] },
              \"scalar ref", bless({ x => 1 }, 'My::Thing'), bless([1, 2], 'My::List'));
    my @out = MT::XSTest::Legacy::echo(@in);
    my @r;
    push @r, scalar(@out) == scalar(@in) ? "count" : "miscount";
    push @r, !defined $out[0] ? "undef" : "defined";
    push @r, join("|", @out[1 .. 4]);
    push @r, $out[5] == 18446744073709551615 ? "uvmax" : "uvlost";
    push @r, $out[6] + $out[7];
    push @r, join("|", @out[8, 9]) . "|" . sprintf("%vd", $out[10]);
    push @r, length($out[11]) == 8 && utf8::is_utf8($out[11]) ? "utf8" : "bytes";
    push @r, $out[12] eq "123" && $out[13] eq "1e3" ? "strings" : "mangled";
    push @r, ($out[14] ? "t" : "f") . ($out[15] ? "t" : "f");
    push @r, $out[16][1][1] + $out[16][2]{a};
    push @r, "$out[17]{k}:$out[17]{n}[1]";
    push @r, ${ $out[18] };
    push @r, ref($out[19]) . ":" . $out[19]{x};
    push @r, ref($out[20]) . ":" . $out[20][1];
    push @r, scalar(MT::XSTest::Legacy::echo(7, 8, 9));   # scalar context: one value
    return join ",", @r;
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/^count,undef,0\|1\|-5\|1099511627776,uvmax,2\.375,\|plain\|99\.97\.102\.233,utf8,strings,tf,4,v:2,scalar ref,My::Thing:1,My::List:2,9$/m,
         'undef, integers, UV max, floats, strings, utf8, booleans, nested arrays and hashes, scalar refs and blessed values round-trip');
}

# ---- big strings: the shm path ------------------------------------------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my @r;
    my $big = MT::XSTest::Legacy::big_string(3_000_000);
    push @r, length($big) == 3_000_000 && $big =~ /^x+$/ ? "3M-in" : "3M-bad";
    my $y = "y" x 2_500_000;
    push @r, MT::XSTest::Legacy::length_of($y) == 2_500_000 ? "2.5M-out" : "2.5M-bad";
    my $z = "z" x 1_000_000;                    # in and back out: 4 MB of one call's arena
    my @e = MT::XSTest::Legacy::echo($z, "small", $z);
    push @r, $e[0] eq $z && $e[2] eq $z && $e[1] eq "small" ? "echo-big" : "echo-bad";
    eval { MT::XSTest::Legacy::length_of("z" x 9_000_000) };
    push @r, $@ =~ /MT_REMOTE_FRAME_TOO_LARGE/ ? "too-large-croaks" : "too-large: " . err($@);
    push @r, MT::XSTest::Legacy::sum(1, 1);     # the bridge still works after it
    return join ",", @r;
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/^3M-in,2\.5M-out,echo-big,too-large-croaks,2$/m,
         'multi-megabyte strings cross through the arena; one over the arena croaks and the bridge survives');
    local $ENV{PERL_MT_XS_ARENA_MB} = 32;
    my $big = child($prelude . <<'CODE');
sub work { require MT::XSTest::Legacy; return MT::XSTest::Legacy::length_of("z" x 9_000_000) }
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($big, qr/^9000000$/m, 'PERL_MT_XS_ARENA_MB sizes the arena');
}

# ---- callbacks through the trampoline ----------------------------------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my @r;
    my @res = MT::XSTest::Legacy::callback(sub { map { $_ * 2 } @_ }, 1, 2, 3);
    push @r, join("+", @res);
    # the callback calls the compartment again: nested
    push @r, MT::XSTest::Legacy::callback_scalar(sub { MT::XSTest::Legacy::sum(@{$_[0]}) }, [4, 5, 6]);
    # a callback that dies is caught inside the compartment ...
    push @r, err(MT::XSTest::Legacy::callback_catch(sub { die "boom\n" }));
    # ... or propagates out to us
    eval { MT::XSTest::Legacy::callback_scalar(sub { die "through\n" }, 0) };
    push @r, err($@);
    # closures keep their lexicals
    my $n = 0;
    MT::XSTest::Legacy::callback(sub { $n += $_[0] }, 10) for 1 .. 5;
    push @r, $n;
    # a callback returning a blessed object and a code ref
    my ($obj, $code) = MT::XSTest::Legacy::callback(sub { (bless({ v => 7 }, 'Host::Obj'), sub { 99 }) });
    push @r, ref($obj) . ":" . $obj->{v} . ":" . $code->();
    push @r, MT::XS::bridge_stats()->{callbacks} >= 9 ? "counted" : "uncounted";
    return join ",", @r;
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/^2\+4\+6,15,boom,through,50,Host::Obj:7:99,counted$/m,
         'host code refs are called from the compartment, nest, keep their lexicals, and their dies go both ways');
}

# ---- croak unwinding ----------------------------------------------------------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my @r;
    eval { MT::XSTest::Legacy::croak_with("plain croak") };
    push @r, err($@);
    eval { MT::XSTest::Legacy::die_object('My::Err') };
    push @r, ref($@) . ":" . $@->{code};
    push @r, MT::XSTest::Legacy::sum(2, 2);          # the stack is intact
    my @list = (1, 2, eval { MT::XSTest::Legacy::croak_with("x"); 3 }, 4);
    push @r, join("", @list);
    return join ",", @r;
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/^plain croak,My::Err:42,4,124$/m,
         'a croak in the compartment is re-raised in the worker, as a string or as the same object');
}

# ---- arguments modified in place are written back ---------------------------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my @r;
    my $x = "old";
    MT::XSTest::Legacy::modify_arg($x, "new");
    push @r, $x;
    my $y = 1;
    MT::XSTest::Legacy::modify_ref(\$y, 5);
    push @r, $y;
    my $z = "keep";
    MT::XSTest::Legacy::echo($z);
    push @r, $z;
    eval { MT::XSTest::Legacy::modify_arg("literal", "x") };   # read-only: no write-back, no croak
    push @r, $@ ? "croaked" : "ok";
    return join ",", @r;
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/^new,5,keep,ok$/m, 'scalars and referents a legacy XSUB assigns to are written back');
}

# ---- remote handles: objects stay in the compartment --------------------------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my @r;
    my $obj = MT::XSTest::Legacy::new_object('MT::XSTest::Legacy::Obj', a => 1, b => "two");
    push @r, ref($obj);
    push @r, MT::XS::is_handle($obj) ? "handle" : "copy";
    push @r, "$obj->{a}:$obj->{b}";                       # the tied shape
    push @r, join("", sort keys %$obj);
    $obj->{c} = 3;
    push @r, MT::XSTest::Legacy::obj_get($obj, "c");      # seen by the compartment
    push @r, exists $obj->{zz} ? "exists" : "absent";
    push @r, MT::XSTest::Legacy::obj_class($obj);
    push @r, MT::XSTest::Legacy::same_object($obj, $obj) ? "same" : "different";
    push @r, $obj->get("a");                              # a method of the .pm half, run over there
    my $arr = MT::XSTest::Legacy::new_array_object('MT::XSTest::Legacy::Obj', 10, 20, 30);
    push @r, "$arr->[1]/" . scalar(@$arr);
    push @r, MT::XSTest::Legacy::objects_alive();
    undef $obj; undef $arr;
    push @r, MT::XSTest::Legacy::objects_alive();         # REL went first in ring order
    my $code = MT::XSTest::Legacy::code_ref();
    push @r, ref($code) . ":" . $code->call(2, 3);
    return join ",", @r;
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/^MT::XSTest::Legacy::Obj,handle,1:two,ab,3,absent,MT::XSTest::Legacy::Obj,same,1,20\/3,2,0,MT::XS::RemoteCode:5$/m,
         'blessed values come back as handles with tied shapes, go back as the same object, and die with their DESTROY');
}

# ---- package variables and proxies ---------------------------------------------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my @r;
    push @r, MT::XSTest::Legacy::pkg_var();
    MT::XS::set_remote_var('$MT::XSTest::Legacy::setting', 'tuned');
    push @r, MT::XSTest::Legacy::pkg_var();
    push @r, MT::XS::get_remote_var('$MT::XSTest::Legacy::setting');
    push @r, $MT::XSTest::Legacy::setting;               # the host's own copy is untouched
    MT::XS::set_remote_var('@MT::XSTest::Legacy::list', [1, 2, 3]);
    push @r, scalar @{ MT::XS::get_remote_var('@MT::XSTest::Legacy::list') };
    my $p = MT::XS::remote('MT::XSTest::Legacy');
    push @r, ref($p);
    push @r, $p->add(1, 2, 3);
    my $o = $p->make(k => "v");
    push @r, ref($o) . ":" . $o->MT_class;
    push @r, $o->{k};
    push @r, $o->set(k => "w")->get("k");
    push @r, $o->class;
    eval { $p->no_such_method };
    push @r, $@ =~ /Can't locate object method "no_such_method"/ ? "nomethod" : err($@);
    return join ",", @r;
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/^default,tuned,tuned,default,3,MT::XS::Proxy,6,MT::XS::Proxy::Obj:MT::XSTest::Legacy::Obj,v,w,MT::XSTest::Legacy::Obj,nomethod$/m,
         'set_remote_var reaches the compartment only; MT::XS::remote proxies class and object methods');
}

# ---- refusals: hooks, magic on host SVs, unsupported types ---------------------------

{
    local $ENV{MT_XSTEST_HOOK} = 1;
    my $out = child($prelude . <<'CODE');
sub work { eval { require MT::XSTest::Legacy }; return err($@) }
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/MT_COMPARTMENT_HOOK_UNSUPPORTED/,
         'a module that installs a compiler hook at boot is refused by the compartment');
}

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my @r;
    my $x = "plain";
    eval { MT::XSTest::Legacy::magic_on_arg($x) };
    push @r, $@ =~ /MT_COMPARTMENT_MAGIC_ON_HOST_SV/ ? "magic-refused" : err($@);
    eval { MT::XSTest::Legacy::echo(\*STDOUT) };
    push @r, $@ =~ /MT_REMOTE_UNSUPPORTED_TYPE/ ? "glob-refused" : err($@);
    eval { MT::XSTest::Legacy::echo(MT::Cap::mint(1)) };
    push @r, $@ =~ /MT_REMOTE_UNSUPPORTED_TYPE/ ? "cap-refused" : err($@);
    my $cycle = []; push @$cycle, $cycle;
    eval { MT::XSTest::Legacy::echo($cycle) };
    push @r, $@ =~ /MT_REMOTE_UNSUPPORTED_TYPE/ ? "cycle-refused" : err($@);
    push @r, MT::XSTest::Legacy::sum(3);
    return join ",", @r;
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/^magic-refused,glob-refused,cap-refused,cycle-refused,3$/m,
         'magic on a host argument, globs, capabilities and cyclic structures are refused and the bridge survives');
}

# ---- %MT::XS::inprocess_ok -------------------------------------------------------------

{
    my $out = child($prelude . <<'CODE');
$MT::XS::inprocess_ok{'MT::XSTest::Legacy'} = 1;
sub work {
    require MT::XSTest::Legacy;
    return (MT::XS::is_remote(\&MT::XSTest::Legacy::sum) ? "remote" : "local") . "," . (MT::XSTest::Legacy::pid() == $$ ? "samepid" : "otherpid");
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/^local,samepid$/m, 'a module vouched for in %MT::XS::inprocess_ok loads in-process in a worker');
}

# ---- compartment death surfaces as an error, handles go stale ------------------------

{
    my $out = child($prelude . <<'CODE');
use Time::HiRes qw(time);
sub work {
    require MT::XSTest::Legacy;
    my @r;
    my $obj = MT::XSTest::Legacy::new_object('MT::XSTest::Legacy::Obj', a => 1);
    my $comp = MT::XS::stats()->{compartment};
    MT::XS::_crash($comp);
    my $t = time;
    eval { MT::XSTest::Legacy::sum(1) };
    push @r, $@ =~ /MT_COMPARTMENT_DEAD/ ? "dead" : err($@);
    push @r, time - $t < 10 ? "fast" : "slow";
    eval { my $v = $obj->{a} };
    push @r, $@ =~ /MT_COMPARTMENT_DEAD|MT_STALE_REMOTE_HANDLE/ ? "stale" : err($@);
    eval { MT::XSTest::Legacy::obj_get($obj, "a") };
    push @r, $@ =~ /MT_COMPARTMENT_DEAD|MT_STALE_REMOTE_HANDLE/ ? "stale2" : err($@);
    my ($c) = grep { $_->{id} == $comp } MT::XS::compartments();
    push @r, "alive=$c->{alive},gen=$c->{gen}";
    return join ",", @r;
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/^dead,fast,stale,stale2,alive=0,gen=2$/m,
         'a compartment dying mid-call is an error in the worker, not a hang; its handles are stale afterwards');
}

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    eval { MT::XSTest::Legacy::exit_now(3) };
    my $e = err($@);
    eval { MT::XSTest::Legacy::sum(1) };
    return "$e | " . err($@);
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/called exit\(\) \(MT_COMPARTMENT_DEAD\) \| .*MT_COMPARTMENT_DEAD/,
         'exit() inside a compartment is reported to the caller and the compartment is gone');
}

# ---- handle affinity: a handle belongs to one compartment ----------------------------

{
    my $out = child($prelude . <<'CODE');
sub work {
    require MT::XSTest::Legacy;
    my $mine = MT::XS::stats()->{compartment};
    my $other = $mine ? 0 : 1;
    my $p = MT::XS::remote('MT::XSTest::Legacy', $other);
    my $o = $p->make(k => "v");                          # lives in the other compartment
    my @r;
    push @r, MT::XS::handle_compartment($o) == $other ? "other" : "same";
    push @r, $o->{k};                                    # its own compartment serves it
    eval { MT::XSTest::Legacy::obj_class($o) };          # my compartment's stub refuses it
    push @r, $@ =~ /MT_REMOTE_HANDLE_AFFINITY/ ? "affinity" : err($@);
    return join ",", @r;
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/^other,v,affinity$/m, 'a handle used with a stub of another compartment croaks MT_REMOTE_HANDLE_AFFINITY');
}

# ---- sixteen workers at once, one compartment ------------------------------------------

{
    local $ENV{PERL_MT_XS_COMPARTMENTS} = 1;
    my $out = child($prelude . <<'CODE');
sub work {
    my ($n) = @_;
    require MT::XSTest::Legacy;
    my $sum = 0;
    for my $i (1 .. $n) {
        $sum += MT::XSTest::Legacy::sum($i, 1);
        MT::XSTest::Legacy::global_bump();
        if ($i % 100 == 0) {
            my $obj = MT::XSTest::Legacy::new_object('MT::XSTest::Legacy::Obj', i => $i);
            $sum += $obj->{i};
            $sum += MT::XSTest::Legacy::callback_scalar(sub { $_[0] + 1 }, $i);
        }
    }
    return $sum;
}
MT::Image::freeze();
my @ids = map { MT::Worker::spawn('main::work', 2000) } 1 .. 16;
my $total = 0; $total += MT::Worker::join($_) for @ids;
say_ "total=$total";
sub tail { require MT::XSTest::Legacy; return MT::XSTest::Legacy::global_bump() . "," . MT::XSTest::Legacy::objects_alive() }
say_ "tail=", run_worker('main::tail');
CODE
    # per worker: sum(i+1) for i in 1..2000 = 2001000+2000 = 2003000; plus 20 objects (i) = 21000; plus 20 callbacks (i+1) = 21020
    like($out, qr/^total=@{[ 16 * (2003000 + 21000 + 21020) ]}$/m,
         'sixteen workers hammer one compartment with calls, objects and callbacks and every result is right');
    like($out, qr/^tail=32001,0$/m, 'every call was served exactly once, every object released');
}

# ---- latency: a trivial remote call ----------------------------------------------------

{
    my $out = child($prelude . <<'CODE');
use Time::HiRes qw(time);
sub work {
    require MT::XSTest::Legacy;
    my $n = $ENV{PERL_MT_LONG_TESTS} ? 1_000_000 : 20_000;
    my @t;
    for my $rep (1 .. 5) {
        my $t = time;
        MT::XSTest::Legacy::sum(1, 2) for 1 .. $n;
        push @t, (time - $t) / $n * 1e6;
    }
    @t = sort { $a <=> $b } @t;
    return sprintf "%d calls x5, median %.2f us, min %.2f us", $n, $t[2], $t[0];
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/^\d+ calls x5, median [\d.]+ us, min [\d.]+ us$/m, 'remote call latency measured');
    diag($out =~ /^(\d+ calls.*)$/m ? $1 : $out);
}

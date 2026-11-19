#!/usr/bin/perl
# perl5-mt: throughput of the hooked operations, scalar and SIMD.
#
# usage: perl tools/simd-bench.pl [--reps N] [--size BYTES]
#
# The level is fixed when a perl starts, so each mode runs in a child perl:
#   scalar  PERL_FORCE_SCALAR=1 (the upstream code)
#   simd    the default level for the host
# The strings are --size bytes (1 MiB by default): pure ASCII, Latin-1 heavy
# (mostly high bytes) and mixed (ASCII with a UTF-8 character every few bytes).
# Each row is the median over --reps runs, in MB/s of the input string.

use strict;
use warnings;
use Time::HiRes qw(time);

my $reps = 5;
my $size = 1 << 20;
my $child;
while (@ARGV) {
    my $opt = shift @ARGV;
    if    ($opt eq '--reps')  { $reps = shift @ARGV }
    elsif ($opt eq '--size')  { $size = shift @ARGV }
    elsif ($opt eq '--child') { $child = 1 }
    else                      { die "usage: $0 [--reps N] [--size BYTES]\n" }
}

# Each entry: row name, and code that returns the seconds one run takes
my @ops = (
    [ 'upgrade', sub {
        my $u = $_[0];
        my $t = time;
        utf8::upgrade($u);
        return time - $t;
    } ],
    [ 'length', sub {
        my $u = $_[0];
        utf8::upgrade($u);
        my $t = time;
        my $n = length $u;
        return time - $t;
    } ],
    [ 'valid', sub {
        my $u = $_[0];
        utf8::decode($u);
        my $t = time;
        my $ok = utf8::valid($u);
        return time - $t;
    } ],
    [ 'decode', sub {
        my $u = $_[0];
        my $t = time;
        utf8::decode($u);
        return time - $t;
    } ],
    [ 'index', sub {
        my $t = time;
        my $at = index($_[0], "BBBBBBBBBBBBBBBB");
        return time - $t;
    } ],
    [ 'tr_upper', sub {
        my $u = $_[0];
        my $t = time;
        $u =~ tr/a-z/A-Z/;
        return time - $t;
    } ],
    [ 'uc', sub {
        my $t = time;
        my $x = uc $_[0];
        return time - $t;
    } ],
    [ 'lc', sub {
        my $t = time;
        my $x = lc $_[0];
        return time - $t;
    } ],
    [ 'split', sub {
        my $t = time;
        my @f = split /,/, $_[0];
        return time - $t;
    } ],
    # regex rows, one node type each: a span over a run of one
    # byte (EXACT), over a run of class members (NANYOFM), and a search that
    # finds one rare byte at the end, for a bitmap class (ANYOF) and a range
    # class (ANYOFR)
    [ 'rx_run', sub {
        my $t = time;
        my $m = $_[0] =~ /^a*/;
        return time - $t;
    } ],
    [ 'rx_span', sub {
        my $t = time;
        my $m = $_[0] =~ /^[^,]*/;
        return time - $t;
    } ],
    [ 'rx_bitmap', sub {
        my $t = time;
        my $m = $_[0] =~ /[QZ]/;
        return time - $t;
    } ],
    [ 'rx_range', sub {
        my $t = time;
        my $m = $_[0] =~ /[XYZ]/;
        return time - $t;
    } ],
    [ 'length_dec', sub {
        my $u = $_[0];
        utf8::decode($u);
        my $t = time;
        my $n = length $u;
        return time - $t;
    } ],
);

sub make_strings {
    my ($n) = @_;
    my $seed = 12345;
    my %s;
    $s{ascii} = 'Aa' x ($n / 2);   # both cases, so uc and lc both change the string
    my $latin = '';
    while (length($latin) < $n) {
        $seed = ($seed * 1103515245 + 12345) % 2147483648;
        $latin .= chr(0x80 + (($seed >> 8) % 128));
    }
    $s{latin1} = substr($latin, 0, $n);
    my $mixed = '';
    while (length($mixed) < $n) {
        $mixed .= 'abcdefghijklmnopqrstuvwxyz' x 3 . "\xC3\xA9" x 2;
    }
    $s{mixed} = substr($mixed, 0, $n);
    # valid UTF-8 text: ASCII runs with 2- and 3-byte characters
    my $text = '';
    while (length($text) < $n) {
        $text .= 'the quick brown fox jumps over the lazy dog ' x 2
              . "\xC3\xA9t\xC3\xA9 " . "\xE2\x82\xAC" x 2;
    }
    $s{utf8} = substr($text, 0, $n);
    # comma-separated ASCII fields of 7 bytes
    my $csv = '';
    while (length($csv) < $n) {
        $csv .= 'abcdefg,';
    }
    $s{csv} = substr($csv, 0, $n);
    # regex rows: a run of one byte, a run of class members, and a long
    # lowercase string with one rare class byte (Z) at the end
    $s{run} = 'a' x $n;
    $s{classrun} = substr('abcdefg' x (int($n / 7) + 1), 0, $n);
    $s{rare} = substr('abcdefghijklmnopqrstuvwxyz' x (int($n / 26) + 1), 0, $n - 1) . 'Z';
    return \%s;
}

sub median {
    my @v = sort { $a <=> $b } @_;
    return $v[int(@v / 2)];
}

# child: print the level, then "kind op mbps" for every string and op
sub run_child {
    my $strings = make_strings($size);
    my $mb = $size / (1 << 20);
    print "level ", MT::SIMD::level_name(), "\n";
    for my $kind (qw(ascii latin1 mixed utf8 csv run classrun rare)) {
        my $src = $strings->{$kind};
        for my $op (@ops) {
            my ($name, $code) = @$op;
            # the regex rows run only on their own strings, and the others not on them
            my %rx_kinds = (rx_run => 'run', rx_span => 'run|classrun',
                            rx_bitmap => 'rare', rx_range => 'rare');
            if (exists $rx_kinds{$name}) {
                next if $kind !~ /^(?:$rx_kinds{$name})$/;
            }
            else {
                next if $kind =~ /^(?:run|classrun|rare)$/;
            }
            # decode, valid and length_dec need UTF-8 text: on a byte string
            # (no UTF-8 flag) they do no work, so those rows are not measured
            next if $name =~ /^(?:decode|valid|length_dec)$/ && $kind !~ /^(?:mixed|utf8)$/;
            # split needs separators, which only the csv string has
            next if ($name eq 'split') != ($kind eq 'csv');
            my @secs;
            for (1 .. $reps) {
                push @secs, $code->($src);
            }
            my $secs = median(@secs);
            printf "%s %s %.1f\n", $kind, $name, $secs > 0 ? $mb / $secs : 0;
        }
    }
}

sub shell_quote {
    my $s = shift;
    $s =~ s/'/'\\''/g;
    return "'$s'";
}

sub run_mode {
    my ($env) = @_;
    local %ENV = %ENV;
    delete $ENV{PERL_FORCE_SCALAR};
    $ENV{PERL_FORCE_SCALAR} = 1 if $env->{scalar};
    my @cmd = ($^X, $0, '--child', '--size', $size, '--reps', $reps);
    my $cmd = join ' ', map { shell_quote($_) } @cmd;
    my $out = `$cmd`;
    die "child perl failed ($?)\n" if $?;
    return $out;
}

if ($child) {
    run_child();
    exit 0;
}

my (%res, %level);
for my $mode (['scalar', { scalar => 1 }], ['simd', {}]) {
    my ($label, $env) = @$mode;
    my $out = run_mode($env);
    for my $line (split /\n/, $out) {
        if ($line =~ /^level (\S+)$/) {
            $level{$label} = $1;
        }
        elsif ($line =~ /^(\S+) (\S+) ([\d.]+)$/) {
            $res{"$1 $2"}{$label} = $3;
        }
    }
}

printf "# %d KiB strings, median of %d runs\n", $size / 1024, $reps;
printf "# simd level: %s (scalar mode: %s)\n", $level{simd}, $level{scalar};
printf "%-18s %12s %12s %8s\n", 'operation', 'scalar MB/s', 'simd MB/s', 'ratio';
for my $key (sort keys %res) {
    my $r = $res{$key};
    printf "%-18s %12.1f %12.1f %8.2f\n", $key, $r->{scalar}, $r->{simd},
        $r->{scalar} > 0 ? $r->{simd} / $r->{scalar} : 0;
}

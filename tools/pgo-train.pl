#!/usr/bin/perl
# PGO training workload driver for perl5-mt.
# Exercises core interpreter tests, t/mt, ext/MT/t, tools/bench/mt.pl,
# and a deterministic pure-Perl hot-path micro-workload.
#
# Usage:
#   ./perl -Ilib tools/pgo-train.pl [--perl=./perl] [--quick] [--workers=4] [--items=2000] [--verbose]

use strict;
use warnings;
use Cwd qw(abs_path getcwd);
use File::Spec;
use Getopt::Long qw(GetOptions);

my $perl_bin = $^X;
my $quick    = 0;
my $workers  = 4;
my $items    = undef;
my $verbose  = 0;
my $help     = 0;

GetOptions(
    'perl=s'    => \$perl_bin,
    'quick!'    => \$quick,
    'workers=i' => \$workers,
    'items=i'   => \$items,
    'verbose!'  => \$verbose,
    'help!'     => \$help,
) or die "Usage: $0 [--perl=PATH] [--quick] [--workers=N] [--items=N] [--verbose]\n";

if ($help) {
    print "Usage: $0 [--perl=PATH] [--quick] [--workers=N] [--items=N] [--verbose]\n";
    exit 0;
}

$items = $quick ? 400 : 2000 unless defined $items;
my $root = getcwd();
my $perl_abs = File::Spec->file_name_is_absolute($perl_bin)
    ? $perl_bin
    : abs_path($perl_bin) // File::Spec->catfile($root, $perl_bin);
my $lib_dir = File::Spec->catdir($root, 'lib');

my $passed = 0;
my $failed = 0;
my $skipped = 0;

sub run_step {
    my ($label, $cwd, $env_extra, @cmd) = @_;
    my $orig = getcwd();
    if (defined $cwd && $cwd ne $orig) {
        chdir $cwd or do {
            warn "pgo-train: cannot chdir($cwd): $!\n";
            $failed++;
            return 0;
        };
    }
    local %ENV = (%ENV, %{ $env_extra || {} });
    print "pgo-train: running $label\n" if $verbose;
    my $rc;
    if ($verbose) {
        $rc = system(@cmd);
    } else {
        open my $oldout, '>&', \*STDOUT or die "dup STDOUT: $!";
        open my $olderr, '>&', \*STDERR or die "dup STDERR: $!";
        open STDOUT, '>', File::Spec->devnull() or die "redirect STDOUT: $!";
        open STDERR, '>', File::Spec->devnull() or die "redirect STDERR: $!";
        $rc = system(@cmd);
        open STDOUT, '>&', $oldout;
        open STDERR, '>&', $olderr;
        close $oldout;
        close $olderr;
    }
    chdir $orig if defined $cwd && $cwd ne $orig;
    if ($rc == 0) {
        $passed++;
        return 1;
    }
    warn sprintf("pgo-train: step '%s' exited with status %d\n", $label, $rc >> 8);
    $failed++;
    return 0;
}

sub run_builtin_hotpaths {
    my ($scale) = @_;
    my $n = $quick ? 15_000 : 80_000;
    $n = int($n * $scale) if defined $scale;

    # 1. Hash insert, lookup, delete, and iteration
    my %h;
    for my $i (1 .. int($n / 4)) {
        my $k = "key_$i";
        $h{$k} = ($i * 2654435761) & 0xFFFFFFFF;
    }
    my $hsum = 0;
    for my $k (keys %h) {
        $hsum += $h{$k} if exists $h{$k};
    }
    for my $i (1 .. int($n / 8)) {
        delete $h{"key_$i"};
    }

    # 2. Array operations and sorting
    my @a = map { ($_ * 31 + 17) % 997 } (1 .. int($n / 5));
    @a = sort { $a <=> $b } @a;
    my $asum = 0;
    $asum += $_ for @a;

    # 3. Regex matching, substitution, and UTF-8 handling
    my $text = "alpha_123 beta_456 gamma_789 \x{00e9}\x{03b1}\x{2603} " x 64;
    my $matches = 0;
    for (1 .. ($quick ? 100 : 500)) {
        $matches++ while $text =~ /([a-z]+)_(\d+)/g;
        (my $copy = $text) =~ s/(\d+)/$1 + 1/ge;
        $matches += length($copy) & 1;
    }

    # 4. Subroutine calls and OO method dispatch
    my $obj = bless { acc => 0 }, 'PGOTrain::Obj';
    my $sub_acc = 0;
    for my $i (1 .. $n) {
        $sub_acc += _hot_add($i, $i & 7);
        $obj->bump($i & 3);
    }

    # 5. Deterministic IEEE 754 floating-point arithmetic
    my $fp = 1.0;
    for my $i (1 .. int($n / 2)) {
        $fp = ($fp * 1.0000001) + ($i * 1e-9);
        $fp -= 1.0 if $fp > 2.0;
    }

    return ($hsum + $asum + $matches + $sub_acc + $obj->{acc} + int($fp)) & 0xFFFF;
}

sub _hot_add {
    my ($x, $y) = @_;
    return ($x ^ $y) + ($x & 15);
}

{
    package PGOTrain::Obj;
    sub bump {
        my ($self, $delta) = @_;
        $self->{acc} += $delta;
        return $self->{acc};
    }
}

print "pgo-train: perl=$perl_abs quick=$quick workers=$workers items=$items\n";

# 1. Core base tests (run from t/ as expected by core Perl tests)
my $t_dir = File::Spec->catdir($root, 't');
if (-d $t_dir) {
    my @core_tests = sort glob(File::Spec->catfile($t_dir, 'base', '*.t'));
    my @op_names = $quick
        ? qw(hash.t array.t sub.t arith.t concat.t)
        : qw(hash.t array.t regexp.t sub.t method.t pack.t sprintf.t sort.t arith.t concat.t eval.t pat.t);
    for my $name (@op_names) {
        my $p = File::Spec->catfile($t_dir, 'op', $name);
        push @core_tests, $p if -f $p;
    }
    for my $t (@core_tests) {
        my $rel = File::Spec->abs2rel($t, $t_dir);
        run_step("core:$rel", $t_dir, { PERL_CORE => 1 }, $perl_abs, '-I../lib', $rel);
    }
} else {
    $skipped++;
}

# 2. t/mt/*.t
my @mt_tests = sort glob(File::Spec->catfile($root, 't', 'mt', '*.t'));
if ($quick && @mt_tests > 6) {
    @mt_tests = @mt_tests[0 .. 5];
}
for my $t (@mt_tests) {
    my $rel = File::Spec->abs2rel($t, $root);
    run_step("mt:$rel", $root, { PERL_MT_LONG_TESTS => 0 }, $perl_abs, '-I.', "-I$lib_dir", $rel);
}

# 3. ext/MT/t/*.t
my @ext_mt_tests = sort glob(File::Spec->catfile($root, 'ext', 'MT', 't', '*.t'));
for my $t (@ext_mt_tests) {
    my $rel = File::Spec->abs2rel($t, $root);
    run_step("ext-mt:$rel", $root, { PERL_MT_LONG_TESTS => 0 }, $perl_abs, '-I.', "-I$lib_dir", $rel);
}

# 4. tools/bench/mt.pl workload
my $bench_mt = File::Spec->catfile($root, 'tools', 'bench', 'mt.pl');
if (-f $bench_mt) {
    run_step("bench:mt.pl", $root, {}, $perl_abs, '-I.', "-I$lib_dir", $bench_mt, $workers, $items);
} else {
    $skipped++;
}

# 5. Built-in hot-path workload
my $digest = run_builtin_hotpaths(1);
$passed++;
print "pgo-train: builtin hot-path digest=$digest\n" if $verbose;

print "pgo-train: complete (passed=$passed failed=$failed skipped=$skipped)\n";
exit($failed > 0 ? 1 : 0);

#!/usr/bin/env perl
#
# tools/bench-suite.pl - Unified reproducible benchmark suite for perl5-mt
#
# Usage:
#   ./perl -I. -Ilib tools/bench-suite.pl [--suite all|1,2,3,4,5,6] [--quick]
#
# Runs six self-contained benchmark suites comparing perl5-mt capabilities
# against scalar/legacy baselines on the same machine, printing Markdown tables:
#   Suite 1: SIMD String, UTF-8 & Regex Kernels (SIMD vs PERL_FORCE_SCALAR=1)
#   Suite 2: SwissTable Hashes vs Chained Hashes (default vs PERL_MT_SWISS=0)
#   Suite 3: Packed MT::Array + Live Hardware Offload (MT::Accel) vs Pure Perl
#   Suite 4: VM Optimizations (default vs PERL_MT_NO_SPECIALIZE=1)
#   Suite 5: Shared-Image Multi-Threading & Fibers vs ithreads vs prefork
#   Suite 6: Parallel Ndarray (MT::PDLBridge) & Concurrency Primitives
#            (MT::Cell CAS, MT::Channel round-trip)

use strict;
use warnings;
use Config;
use File::Spec;
use FindBin;
use Time::HiRes qw(time);

my $ROOT = File::Spec->rel2abs(File::Spec->catdir($FindBin::Bin, File::Spec->updir));
my $SEP  = $Config{path_sep} || ':';

my $suite_spec = 'all';
my $quick      = 0;
my $child_mode = '';

while (@ARGV) {
    my $opt = shift @ARGV;
    if    ($opt eq '--suite') { $suite_spec = shift @ARGV }
    elsif ($opt eq '--quick') { $quick      = 1 }
    elsif ($opt eq '--child') { $child_mode = shift @ARGV }
    elsif ($opt eq '--help' || $opt eq '-h') {
        print "Usage: $^X -I. -Ilib $0 [--suite all|1,2,3,4,5,6] [--quick]\n";
        exit 0;
    }
    else {
        die "Unknown option: $opt\n";
    }
}

# ---- Shared helpers -------------------------------------------------------

sub median {
    my @v = sort { $a <=> $b } @_;
    return 0 unless @v;
    my $n = scalar @v;
    return ($n % 2) ? $v[int($n / 2)] : 0.5 * ($v[$n / 2 - 1] + $v[$n / 2]);
}

sub dhex {
    return unpack('H16', pack('d>', $_[0]));
}

sub build_perl5lib {
    my @dirs;
    my %seen;
    for my $d ($ROOT, File::Spec->catdir($ROOT, 'lib'), @INC) {
        next unless defined $d && length $d && !ref($d);
        next if $seen{$d}++;
        push @dirs, $d;
    }
    return join($SEP, @dirs);
}

sub run_child_perl {
    my ($env_overrides, @args) = @_;
    local %ENV = %ENV;
    $ENV{PERL5LIB} = build_perl5lib();
    for my $k (keys %$env_overrides) {
        if (defined $env_overrides->{$k}) {
            $ENV{$k} = $env_overrides->{$k};
        }
        else {
            delete $ENV{$k};
        }
    }
    my @cmd = ($^X, '-I' . $ROOT, '-I' . File::Spec->catdir($ROOT, 'lib'), @args);
    open my $fh, '-|', @cmd or die "Failed to exec @cmd: $!\n";
    local $/;
    my $out = <$fh>;
    close $fh;
    if ($? != 0) {
        my $rc  = $? >> 8;
        my $sig = $? & 127;
        die "Child command failed (exit=$rc, sig=$sig): @cmd\nOutput:\n$out\n";
    }
    return $out // '';
}

sub detect_hardware {
    my %hw = (
        os       => $^O,
        arch     => $Config{archname},
        cpu      => 'unknown',
        cores    => 0,
        mem_gib  => 'unknown',
        os_ver   => '',
        cc       => $Config{cc} || 'cc',
        cc_ver   => '',
        optimize => $Config{optimize} || '',
        commit   => 'unknown',
    );

    if ($^O eq 'darwin') {
        chomp(my $brand = `sysctl -n machdep.cpu.brand_string 2>/dev/null`);
        chomp(my $cores = `sysctl -n hw.physicalcpu 2>/dev/null`);
        chomp(my $mem   = `sysctl -n hw.memsize 2>/dev/null`);
        chomp(my $prod  = `sw_vers -productName 2>/dev/null`);
        chomp(my $pver  = `sw_vers -productVersion 2>/dev/null`);
        $hw{cpu}     = $brand if length $brand;
        $hw{cores}   = $cores + 0 if $cores =~ /^\d+$/;
        $hw{mem_gib} = sprintf('%.0f GiB', $mem / (1024 ** 3)) if $mem =~ /^\d+$/ && $mem > 0;
        $hw{os_ver}  = "$prod $pver" if length $pver;
    }
    elsif ($^O eq 'linux') {
        if (open my $fh, '<', '/proc/cpuinfo') {
            while (my $line = <$fh>) {
                if ($line =~ /^(?:model name|cpu|Model)\s*:\s*(.+)/i) {
                    $hw{cpu} = $1;
                    $hw{cpu} =~ s/\s+$//;
                    last;
                }
            }
            close $fh;
        }
        chomp(my $cores = `nproc 2>/dev/null`);
        $hw{cores} = $cores + 0 if $cores =~ /^\d+$/;
        if (open my $fh, '<', '/proc/meminfo') {
            while (my $line = <$fh>) {
                if ($line =~ /^MemTotal:\s+(\d+)\s+kB/i) {
                    $hw{mem_gib} = sprintf('%.0f GiB', $1 / (1024 * 1024));
                    last;
                }
            }
            close $fh;
        }
        chomp(my $uname = `uname -sr 2>/dev/null`);
        $hw{os_ver} = $uname if length $uname;
    }
    elsif ($^O eq 'aix') {
        chomp(my $uname = `uname -srvm 2>/dev/null`);
        $hw{os_ver} = $uname if length $uname;
        my $lp = `lparstat -i 2>/dev/null`;
        if (defined $lp && $lp =~ /Online Virtual CPUs\s*:\s*(\d+)/) {
            $hw{cores} = $1 + 0;
        }
    }
    else {
        chomp(my $uname = `uname -sr 2>/dev/null`);
        $hw{os_ver} = $uname if length $uname;
    }

    my $cc_out = `$hw{cc} --version 2>/dev/null`;
    if (defined $cc_out && $cc_out =~ /^([^\r\n]+)/) {
        $hw{cc_ver} = $1;
    }

    my $git_sha = `git -C "$ROOT" rev-parse --short HEAD 2>/dev/null`;
    if (defined $git_sha && $git_sha =~ /^([0-9a-f]+)/i) {
        $hw{commit} = $1;
    }

    return \%hw;
}

# ---- Child workers for isolated environment measurements ------------------

if ($child_mode eq 'suite2') {
    my $n    = $quick ? 40_000 : 100_000;
    my $reps = $quick ? 3 : 5;

    my @keys      = map { sprintf('key_%07d', $_) } 0 .. $n - 1;
    my @miss_keys = map { sprintf('miss_%07d', $_) } 0 .. $n - 1;

    my (%times, $checksum);
    for my $rep (1 .. $reps) {
        my %h;
        my $t0 = time();
        for my $i (0 .. $n - 1) {
            $h{$keys[$i]} = $i;
        }
        push @{ $times{insert} }, time() - $t0;

        my $hit_sum = 0;
        my $t1 = time();
        for my $k (@keys) {
            $hit_sum += $h{$k};
        }
        push @{ $times{hit_lookup} }, time() - $t1;

        my $miss_cnt = 0;
        my $t2 = time();
        for my $k (@miss_keys) {
            $miss_cnt++ if exists $h{$k};
        }
        push @{ $times{miss_lookup} }, time() - $t2;

        my $iter_sum = 0;
        my $t3 = time();
        while (my ($k, $v) = each %h) {
            $iter_sum += $v;
        }
        push @{ $times{iterate} }, time() - $t3;

        my $t4 = time();
        for my $k (@keys) {
            delete $h{$k};
        }
        push @{ $times{delete} }, time() - $t4;

        $checksum = "$hit_sum:$miss_cnt:$iter_sum:" . scalar(keys %h);
    }

    print "CHECKSUM $checksum\n";
    for my $op (qw(insert hit_lookup miss_lookup iterate delete)) {
        my $med = median(@{ $times{$op} });
        my $mops = $med > 0 ? ($n / $med) / 1e6 : 0;
        printf "OP %s %.6f %.3f\n", $op, $med, $mops;
    }
    exit 0;
}

if ($child_mode eq 'suite3_pure' || $child_mode eq 'suite3_mt') {
    require MT::Array;
    require MT::Accel;

    my $n     = $quick ? 65_536 : 131_072;
    my $reps  = $quick ? 3 : 5;
    my $alpha = -2.375;

    my $seed = 0x51a7c0de;
    my (@a_raw, @b_raw);
    $#a_raw = $n - 1;
    $#b_raw = $n - 1;
    for my $i (0 .. $n - 1) {
        $seed = ($seed * 1103515245 + 12345) & 0x7fffffff;
        my $e1 = ($seed % 20) - 10;
        my $m1 = (($seed % 20001) - 10000) / 10000.0;
        $a_raw[$i] = $m1 * (2.0 ** $e1);

        $seed = ($seed * 1103515245 + 12345) & 0x7fffffff;
        my $e2 = ($seed % 20) - 10;
        my $m2 = (($seed % 20001) - 10000) / 10000.0;
        $b_raw[$i] = $m2 * (2.0 ** $e2);
    }

    my @u8_a_raw = map { ($_ * 37) & 0xff } 0 .. $n - 1;
    my @u8_b_raw = map { ($_ * 53 + 7) & 0xff } 0 .. $n - 1;

    if ($child_mode eq 'suite3_pure') {
        my @i8_a = map { $_ >= 128 ? $_ - 256 : $_ } @u8_a_raw;
        my @i8_b = map { $_ >= 128 ? $_ - 256 : $_ } @u8_b_raw;

        my %ops = (
            sum => sub {
                my $s = 0.0;
                $s += $_ for @a_raw;
                return dhex($s);
            },
            dot => sub {
                my $s = 0.0;
                for my $i (0 .. $n - 1) {
                    $s += $a_raw[$i] * $b_raw[$i];
                }
                return dhex($s);
            },
            vec_add => sub {
                my @out;
                $#out = $n - 1;
                for my $i (0 .. $n - 1) {
                    $out[$i] = $a_raw[$i] + $b_raw[$i];
                }
                my $s = 0.0;
                $s += $_ for @out;
                return dhex($s);
            },
            vec_mul => sub {
                my @out;
                $#out = $n - 1;
                for my $i (0 .. $n - 1) {
                    $out[$i] = $a_raw[$i] * $b_raw[$i];
                }
                my $s = 0.0;
                $s += $_ for @out;
                return dhex($s);
            },
            axpy => sub {
                my @out;
                $#out = $n - 1;
                for my $i (0 .. $n - 1) {
                    $out[$i] = $alpha * $a_raw[$i] + $b_raw[$i];
                }
                my $s = 0.0;
                $s += $_ for @out;
                return dhex($s);
            },
            dot_i8 => sub {
                my $s = 0;
                for my $i (0 .. $n - 1) {
                    $s += $i8_a[$i] * $i8_b[$i];
                }
                return sprintf('%d', $s);
            },
        );

        for my $name (qw(sum dot vec_add vec_mul axpy dot_i8)) {
            my @secs;
            my $hex = '';
            for (1 .. $reps) {
                my $t0 = time();
                $hex = $ops{$name}->();
                push @secs, time() - $t0;
            }
            printf "OP %s %.6f %s\n", $name, median(@secs), $hex;
        }
        exit 0;
    }

    # suite3_mt: packed MT::Array with active MT::Accel or PERL_ACCEL=off
    my $a    = MT::Array::packed('NV', @a_raw);
    my $b    = MT::Array::packed('NV', @b_raw);
    my $u8_a = MT::Array::packed('U8', @u8_a_raw);
    my $u8_b = MT::Array::packed('U8', @u8_b_raw);

    # Warmup once
    MT::Array::sum($a);
    MT::Accel::reset_stats();

    # Inner loop counts keep each timed interval well above timer jitter
    my $inner = $quick ? 5 : 10;

    my %bench = (
        sum => sub {
            my $r;
            $r = MT::Array::sum($a) for 1 .. $inner;
            return dhex($r);
        },
        dot => sub {
            my $r;
            $r = MT::Array::dot($a, $b) for 1 .. $inner;
            return dhex($r);
        },
        vec_add => sub {
            my $v;
            $v = MT::Array::vec_add($a, $b) for 1 .. $inner;
            return $v;
        },
        vec_mul => sub {
            my $v;
            $v = MT::Array::vec_mul($a, $b) for 1 .. $inner;
            return $v;
        },
        axpy => sub {
            my $v;
            $v = MT::Array::axpy($alpha, $a, $b) for 1 .. $inner;
            return $v;
        },
        dot_i8 => sub {
            my $r;
            $r = MT::Array::dot_i8($u8_a, $u8_b) for 1 .. $inner;
            return sprintf('%d', $r);
        },
    );

    my %hex_out;
    for my $name (qw(sum dot vec_add vec_mul axpy dot_i8)) {
        my @secs;
        my $last;
        for (1 .. $reps) {
            my $t0 = time();
            $last = $bench{$name}->();
            push @secs, (time() - $t0) / $inner;
        }
        if (ref($last) eq 'ARRAY') {
            $hex_out{$name} = dhex(MT::Array::sum($last));
        }
        else {
            $hex_out{$name} = $last;
        }
        printf "OP %s %.6f %s\n", $name, median(@secs), $hex_out{$name};
    }
    my $st = MT::Accel::stats();
    printf "META active=%s offload_calls=%d fallback_calls=%d last_backend=%s nelem=%d\n",
        MT::Accel::active(),
        $st->{offload_calls} // 0,
        $st->{fallback_calls} // 0,
        $st->{last_backend} // 'none',
        $n;
    exit 0;
}

# ---- Suite 4 & 5b & 6 top-level frozen worker routines --------------------

{
    package BenchBase;
    sub calc { my ($self, $x) = @_; return ($x & 255) + 1 }

    package BenchSub;
    our @ISA = ('BenchBase');
    sub tag { return 7 }
}

our %FROZEN_CFG = (alpha => 11, beta => 23, gamma => 37);
sub CONST_ANS { 42 }
sub add_two { my ($x, $y) = @_; return $x + $y }

sub suite4_worker {
    my ($iters, $nreps) = @_;
    my $obj = bless {}, 'BenchSub';
    my (%res, %chk);

    # 1. Frozen constant sub (spec_const_subs)
    {
        my @s;
        my $acc = 0;
        for (1 .. $nreps) {
            $acc = 0;
            my $t0 = time();
            my $i = 0;
            while ($i < $iters) {
                my $c = CONST_ANS();
                $acc += $c;
                $i++;
            }
            push @s, time() - $t0;
        }
        $res{const_sub} = median(@s);
        $chk{const_sub} = $acc;
    }

    # 2. Static class method devirtualization (spec_meth_devirt)
    {
        my @s;
        my $acc = 0;
        for (1 .. $nreps) {
            $acc = 0;
            my $t0 = time();
            my $i = 0;
            while ($i < $iters) {
                $acc += BenchSub->tag();
                $i++;
            }
            push @s, time() - $t0;
        }
        $res{static_method} = median(@s);
        $chk{static_method} = $acc;
    }

    # 3. Object method inline cache (meth_ic)
    {
        my @s;
        my $acc = 0;
        for (1 .. $nreps) {
            $acc = 0;
            my $t0 = time();
            my $i = 0;
            while ($i < $iters) {
                $acc += $obj->calc($i);
                $i++;
            }
            push @s, time() - $t0;
        }
        $res{obj_method} = median(@s);
        $chk{obj_method} = $acc;
    }

    # 4. Frozen constant-key hash inline cache (helem_ic)
    {
        my @s;
        my $acc = 0;
        for (1 .. $nreps) {
            $acc = 0;
            my $t0 = time();
            my $i = 0;
            while ($i < $iters) {
                $acc += $FROZEN_CFG{alpha};
                $i++;
            }
            push @s, time() - $t0;
        }
        $res{frozen_helem} = median(@s);
        $chk{frozen_helem} = $acc;
    }

    # 5. Mixed hot loop (const sub + static method + helem + sub call + superinstruction loop)
    {
        my $mn = int($iters / 4);
        my @s;
        my $acc = 0;
        for (1 .. $nreps) {
            $acc = 0;
            my $t0 = time();
            my $i = 0;
            while ($i < $mn) {
                my $c = CONST_ANS();
                $acc += add_two($c, $FROZEN_CFG{alpha}) + BenchSub->tag();
                $i++;
            }
            push @s, time() - $t0;
        }
        $res{mixed_hot_loop} = median(@s);
        $chk{mixed_hot_loop} = $acc;
    }

    my @lines;
    for my $k (qw(const_sub static_method obj_method frozen_helem mixed_hot_loop)) {
        my $cnt = $k eq 'mixed_hot_loop' ? int($iters / 4) : $iters;
        my $mops = $res{$k} > 0 ? ($cnt / $res{$k}) / 1e6 : 0;
        push @lines, sprintf("OP %s %.6f %.3f %s", $k, $res{$k}, $mops, $chk{$k});
    }
    return join("\n", @lines);
}

sub suite5_fiber_worker {
    my ($spec) = @_;
    my ($start_i, $count, $nfibers) = split /:/, $spec;
    my $per = int(($count + $nfibers - 1) / $nfibers);
    my @fids;
    for my $f (0 .. $nfibers - 1) {
        my $lo = $start_i + $f * $per;
        my $hi = $lo + $per - 1;
        $hi = $start_i + $count - 1 if $hi >= $start_i + $count;
        next if $lo > $hi;
        push @fids, MT::Fiber::schedule(sub {
            my $acc = 0;
            for my $i ($lo .. $hi) {
                my $h = 2166136261;
                for my $k (1 .. 1500) {
                    $h ^= ($k * ($i + 1)) & 0xffff;
                    $h = ($h * 16777619) & 0xffffffff;
                }
                $acc += $h & 0xff;
                MT::Fiber::pass() if ($i & 3) == 0;
            }
            return $acc;
        });
    }
    MT::Fiber::run();
    my $total = 0;
    $total += MT::Fiber::join($_) for @fids;
    return $total;
}

sub cell_cas_worker {
    my ($cid, $iters) = @_;
    my $cell = MT::Cell->attach($cid);
    my $t0 = time();
    for my $i (0 .. $iters - 1) {
        $cell->cas($i, $i + 1) or die "CAS failed at $i\n";
    }
    return time() - $t0;
}

sub cell_get_worker {
    my ($cid, $iters) = @_;
    my $cell = MT::Cell->attach($cid);
    my $sum = 0;
    my $t0 = time();
    for (1 .. $iters) {
        $sum += $cell->get();
    }
    return sprintf("%.6f:%d", time() - $t0, $sum);
}

sub chan_pong_worker {
    my ($in_id, $out_id, $iters) = @_;
    my $in  = MT::Channel->attach($in_id);
    my $out = MT::Channel->attach($out_id);
    for (1 .. $iters) {
        my $v = $in->recv();
        $out->send($v + 1);
    }
    return 1;
}

if ($child_mode eq 'suite4') {
    my $n    = $quick ? 500_000 : 2_000_000;
    my $reps = $quick ? 3 : 5;

    MT::Image::freeze();
    my $st = MT::Image::stats();
    printf "STATS specialize=%d meth_ic=%d helem_ic=%d spec_const=%d spec_devirt=%d\n",
        $st->{specialize} // 0,
        $st->{n_meth_ic} // 0,
        $st->{n_helem_ic} // 0,
        $st->{n_spec_const_subs} // 0,
        $st->{n_spec_meth_devirt} // 0;

    my $wid = MT::Worker::spawn('main::suite4_worker', $n, $reps);
    my $out = MT::Worker::join($wid);
    print "$out\n";
    exit 0;
}

if ($child_mode eq 'suite5_fibers') {
    require MT::Pool;
    require MT::Fiber;

    my $items   = $quick ? 400 : 2000;
    my $nfibers = 4;
    my @workers = $quick ? (1, 2, 4) : (1, 2, 4, 8);
    my $reps    = $quick ? 1 : 3;

    MT::Image::freeze();

    # Fiber resume+yield context-switch microbenchmark
    my $sw_n = $quick ? 50_000 : 200_000;
    my $fid = MT::Fiber::spawn(sub {
        my $i = 0;
        while ($i < $sw_n) {
            MT::Fiber::yield($i);
            $i++;
        }
        return $i;
    });
    my $t_sw0 = time();
    for (1 .. $sw_n) {
        MT::Fiber::resume($fid);
    }
    MT::Fiber::resume($fid);
    my $t_sw = time() - $t_sw0;
    MT::Fiber::join($fid);
    printf "FIBER_SWITCH %.1f %.0f %d\n", ($t_sw / $sw_n) * 1e9, $sw_n / $t_sw, $sw_n;

    # MT::Pool with 4 cooperative fibers per worker
    for my $w (@workers) {
        my @ops;
        for (1 .. $reps) {
            my $pool = MT::Pool->new(workers => $w);
            my $chunk = int(($items + $w - 1) / $w);
            my @specs;
            for my $k (0 .. $w - 1) {
                my $lo = 1 + $k * $chunk;
                my $cnt = $chunk;
                $cnt = $items - $lo + 1 if $lo + $cnt - 1 > $items;
                next if $cnt <= 0;
                push @specs, "$lo:$cnt:$nfibers";
            }
            my $t0 = time();
            $pool->map(\&suite5_fiber_worker, @specs);
            my $dt = time() - $t0;
            $pool->shutdown();
            push @ops, $dt > 0 ? $items / $dt : 0;
        }
        printf "FIBER_POOL %d %d %.1f\n", $w, $nfibers, median(@ops);
    }
    exit 0;
}

if ($child_mode eq 'suite6_conc') {
    require MT::Cell;
    require MT::Channel;

    my $cas_n  = $quick ? 100_000 : 250_000;
    my $ping_n = $quick ? 10_000  : 50_000;
    my $reps   = $quick ? 3 : 5;

    MT::Image::freeze();

    # 1. MT::Cell lock-free CAS ops/s
    my @cas_secs;
    for (1 .. $reps) {
        my $cell = MT::Cell->new(0);
        my $wid  = MT::Worker::spawn('main::cell_cas_worker', $cell->id, $cas_n);
        my $dt   = MT::Worker::join($wid);
        die "Cell final mismatch" unless $cell->get() == $cas_n;
        push @cas_secs, $dt;
    }
    my $cas_med  = median(@cas_secs);
    my $cas_ops  = $cas_med > 0 ? $cas_n / $cas_med : 0;

    # 2. MT::Cell lock-free get ops/s
    my @get_secs;
    for (1 .. $reps) {
        my $cell = MT::Cell->new(7);
        my $wid  = MT::Worker::spawn('main::cell_get_worker', $cell->id, $cas_n);
        my ($dt, $sum) = split /:/, MT::Worker::join($wid);
        die "Cell get sum mismatch" unless $sum == 7 * $cas_n;
        push @get_secs, $dt;
    }
    my $get_med  = median(@get_secs);
    my $get_ops  = $get_med > 0 ? $cas_n / $get_med : 0;

    # 3. MT::Channel cross-worker ping-pong round-trip latency (us)
    my @rtt_us;
    for (1 .. $reps) {
        my $c_req  = MT::Channel->new(1);
        my $c_resp = MT::Channel->new(1);
        my $wid    = MT::Worker::spawn('main::chan_pong_worker', $c_req->id, $c_resp->id, $ping_n);
        # Warmup one round trip
        my $t0 = time();
        for my $i (1 .. $ping_n) {
            $c_req->send($i);
            my $r = $c_resp->recv();
            die "Channel parity mismatch" unless $r == $i + 1;
        }
        my $dt = time() - $t0;
        MT::Worker::join($wid);
        push @rtt_us, ($dt / $ping_n) * 1e6;
    }
    my $rtt_med = median(@rtt_us);
    my $rtt_ops = $rtt_med > 0 ? 1e6 / $rtt_med : 0;

    printf "CELL_CAS %.0f %.3f %d\n", $cas_ops, ($cas_med / $cas_n) * 1e6, $cas_n;
    printf "CELL_GET %.0f %.3f %d\n", $get_ops, ($get_med / $cas_n) * 1e6, $cas_n;
    printf "CHAN_RTT %.2f %.0f %d\n", $rtt_med, $rtt_ops, $ping_n;
    exit 0;
}

# ---- Parent orchestrator --------------------------------------------------

my %want;
if ($suite_spec eq 'all') {
    %want = map { $_ => 1 } (1 .. 6);
}
else {
    for my $s (split /,/, $suite_spec) {
        $s =~ s/\s+//g;
        die "Invalid suite number '$s' (expected 1..6 or all)\n" unless $s =~ /^[1-6]$/;
        $want{$s} = 1;
    }
}

my $hw = detect_hardware();
my $simd_lvl = eval { MT::SIMD::level_name() } || 'unknown';
my $accel_bk = eval { require MT::Accel; MT::Accel::active() } || 'unknown';

print "# perl5-mt Unified Benchmark Suite Results\n\n";
print "- **Host**: `$hw->{os}` (`$hw->{os_ver}`), `$hw->{arch}`\n";
print "- **CPU**: $hw->{cpu} ($hw->{cores} physical cores, $hw->{mem_gib} RAM)\n";
print "- **Compiler**: `$hw->{cc_ver}` (`$hw->{optimize}`)\n";
print "- **Perl**: `$^V` (commit `$hw->{commit}`)\n";
print "- **Active SIMD / Accel**: SIMD=`$simd_lvl`, Accel=`$accel_bk`\n";
print "- **Command**: `./perl -I. -Ilib tools/bench-suite.pl" . ($quick ? " --quick" : "") . "`\n\n";

# ---- Suite 1: SIMD String, UTF-8 & Regex Kernels --------------------------

if ($want{1}) {
    print "## Suite 1: SIMD String, UTF-8 & Regex Kernels\n\n";
    my $reps = $quick ? 3 : 5;
    my $size = $quick ? 262144 : 1048576;
    my $script = File::Spec->catfile($ROOT, 'tools', 'simd-bench.pl');
    my $out = run_child_perl({}, $script, '--reps', $reps, '--size', $size);

    my ($simd_mode, $scalar_mode) = ('unknown', 'scalar');
    my @rows;
    my $log_sum = 0;
    my $max_ratio = 0;
    my $max_op = '';

    for my $line (split /\n/, $out) {
        if ($line =~ /^# simd level:\s*(\S+)\s+\(scalar mode:\s*(\S+)\)/) {
            ($simd_mode, $scalar_mode) = ($1, $2);
        }
        elsif ($line =~ /^(\S+\s+\S+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)$/) {
            my ($op, $sc, $si, $rat) = ($1, $2, $3, $4);
            push @rows, [$op, $sc, $si, $rat];
            if ($rat > 0) {
                $log_sum += log($rat);
                if ($rat > $max_ratio) {
                    $max_ratio = $rat;
                    $max_op = $op;
                }
            }
        }
    }

    my $geomean = @rows ? exp($log_sum / scalar(@rows)) : 0;
    printf "Strings: %d KiB, median of %d runs. Active SIMD tier: `%s` vs `%s` (`PERL_FORCE_SCALAR=1`).\n\n",
        $size / 1024, $reps, $simd_mode, $scalar_mode;
    print "| Operation | Scalar (MB/s) | SIMD (MB/s) | Speedup |\n";
    print "|---|---:|---:|---:|\n";
    for my $r (@rows) {
        printf "| `%s` | %.1f | %.1f | **%.2fx** |\n", $r->[0], $r->[1], $r->[2], $r->[3];
    }
    printf "\n*Summary: %d kernels measured; geometric mean **%.2fx**, peak **%.2fx** (`%s`).*\n\n",
        scalar(@rows), $geomean, $max_ratio, $max_op;
}

# ---- Suite 2: SwissTable Hashes vs Chained Hashes -------------------------

if ($want{2}) {
    print "## Suite 2: SwissTable Hashes vs Chained Hashes\n\n";
    my @base_args = ($0, '--child', 'suite2');
    push @base_args, '--quick' if $quick;

    my $out_chain = run_child_perl({ PERL_MT_SWISS => 0 }, @base_args);
    my $out_swiss = run_child_perl({ PERL_MT_SWISS => 1 }, @base_args);

    my (%chain, %swiss, $chk_c, $chk_s);
    for my $line (split /\n/, $out_chain) {
        $chk_c = $1 if $line =~ /^CHECKSUM (\S+)/;
        $chain{$1} = [$2, $3] if $line =~ /^OP (\S+) ([\d.]+) ([\d.]+)/;
    }
    for my $line (split /\n/, $out_swiss) {
        $chk_s = $1 if $line =~ /^CHECKSUM (\S+)/;
        $swiss{$1} = [$2, $3] if $line =~ /^OP (\S+) ([\d.]+) ([\d.]+)/;
    }

    my $parity = (defined $chk_c && defined $chk_s && $chk_c eq $chk_s) ? 'PASS' : 'FAIL';
    my $nkeys = $quick ? '40,000' : '100,000';
    print "Hash size: $nkeys keys, median of " . ($quick ? 3 : 5) . " runs (`PERL_MT_SWISS=1` vs `PERL_MT_SWISS=0`). Checksum parity: **$parity**.\n\n";
    print "| Operation | Chained (Mops/s) | SwissTable (Mops/s) | Speedup |\n";
    print "|---|---:|---:|---:|\n";
    for my $op (qw(insert hit_lookup miss_lookup iterate delete)) {
        my $cm = $chain{$op}[1] || 0;
        my $sm = $swiss{$op}[1] || 0;
        my $sp = $cm > 0 ? $sm / $cm : 0;
        printf "| `%s` | %.2f | %.2f | **%.2fx** |\n", $op, $cm, $sm, $sp;
    }
    print "\n";
}

# ---- Suite 3: Packed MT::Array + Live Hardware Offload --------------------

if ($want{3}) {
    print "## Suite 3: Packed `MT::Array` + Live Hardware Offload (`MT::Accel`) vs Pure-Perl Arrays\n\n";
    my @qflag = $quick ? ('--quick') : ();
    my $out_pure = run_child_perl({}, $0, '--child', 'suite3_pure', @qflag);
    my $out_off  = run_child_perl({ PERL_ACCEL => 'off' }, $0, '--child', 'suite3_mt', @qflag);
    my $out_live = run_child_perl({ PERL_ACCEL => 'auto' }, $0, '--child', 'suite3_mt', @qflag);

    my (%pure, %off, %live, %meta_live);
    for my $line (split /\n/, $out_pure) {
        $pure{$1} = [$2, $3] if $line =~ /^OP (\S+) ([\d.]+) (\S+)/;
    }
    for my $line (split /\n/, $out_off) {
        $off{$1} = [$2, $3] if $line =~ /^OP (\S+) ([\d.]+) (\S+)/;
    }
    for my $line (split /\n/, $out_live) {
        $live{$1} = [$2, $3] if $line =~ /^OP (\S+) ([\d.]+) (\S+)/;
        if ($line =~ /^META (.+)/) {
            for my $kv (split /\s+/, $1) {
                my ($k, $v) = split /=/, $kv, 2;
                $meta_live{$k} = $v;
            }
        }
    }

    printf "Vector length: %d elements, median of %d runs. Active backend: `%s` (`offload_calls=%d`, `last_backend=%s`).\n\n",
        $meta_live{nelem} || 0, ($quick ? 3 : 5),
        $meta_live{active} || 'unknown',
        $meta_live{offload_calls} || 0,
        $meta_live{last_backend} || 'none';

    print "| Operation | Pure-Perl (ms) | `MT::Array` Scalar CR (ms) | `MT::Array` + `MT::Accel` (ms) | Speedup vs Pure-Perl | Accel vs Scalar CR | Bit-Identity (Scalar CR vs Accel) |\n";
    print "|---|---:|---:|---:|---:|---:|---|\n";
    for my $op (qw(sum dot vec_add vec_mul axpy dot_i8)) {
        my $tp = ($pure{$op}[0] || 0) * 1000;
        my $to = ($off{$op}[0]  || 0) * 1000;
        my $tl = ($live{$op}[0] || 0) * 1000;
        my $sp_pp = $tl > 0 ? $tp / $tl : 0;
        my $sp_sc = $tl > 0 ? $to / $tl : 0;
        my $ho = $off{$op}[1]  // '';
        my $hl = $live{$op}[1] // '';
        my $id = ($ho eq $hl && length $ho) ? "EXACT (`$hl`)" : "DIFF (`$ho` vs `$hl`)";
        printf "| `%s` | %.3f | %.3f | %.3f | **%.1fx** | **%.2fx** | %s |\n",
            $op, $tp, $to, $tl, $sp_pp, $sp_sc, $id;
    }
    print "\n";
}

# ---- Suite 4: VM Optimizations --------------------------------------------

if ($want{4}) {
    print "## Suite 4: VM Optimizations (Freeze-Time Specialisation & Inline Caches)\n\n";
    my @base_args = ($0, '--child', 'suite4');
    push @base_args, '--quick' if $quick;

    my $out_off = run_child_perl({ PERL_MT_NO_SPECIALIZE => 1 }, @base_args);
    my $out_on  = run_child_perl({ PERL_MT_NO_SPECIALIZE => undef }, @base_args);

    my (%off, %on, $stats_on);
    for my $line (split /\n/, $out_off) {
        $off{$1} = [$2, $3, $4] if $line =~ /^OP (\S+) ([\d.]+) ([\d.]+) (\S+)/;
    }
    for my $line (split /\n/, $out_on) {
        $stats_on = $1 if $line =~ /^STATS (.+)/;
        $on{$1} = [$2, $3, $4] if $line =~ /^OP (\S+) ([\d.]+) ([\d.]+) (\S+)/;
    }

    print "Tight loops inside a frozen `MT::Image` worker comparing default specialisation (`$stats_on`) vs `PERL_MT_NO_SPECIALIZE=1`.\n\n";
    print "| Workload | `PERL_MT_NO_SPECIALIZE=1` (Mops/s) | Default Specialized (Mops/s) | Speedup | Parity |\n";
    print "|---|---:|---:|---:|---|\n";
    for my $op (qw(const_sub static_method obj_method frozen_helem mixed_hot_loop)) {
        my $m_off = $off{$op}[1] || 0;
        my $m_on  = $on{$op}[1]  || 0;
        my $sp    = $m_off > 0 ? $m_on / $m_off : 0;
        my $par   = (($off{$op}[2] // '') eq ($on{$op}[2] // '')) ? 'PASS' : 'FAIL';
        printf "| `%s` | %.2f | %.2f | **%.2fx** | %s |\n", $op, $m_off, $m_on, $sp, $par;
    }
    print "\n";
}

# ---- Suite 5: Shared-Image Multi-Threading & Fibers vs ithreads vs prefork

if ($want{5}) {
    print "## Suite 5: Shared-Image Multi-Threading & Fibers vs `ithreads` vs `prefork`\n\n";
    my $img_mb = $quick ? 16 : 32;
    my $items  = $quick ? 80 : 200;
    my $reps   = $quick ? 1  : 3;
    my @workers = $quick ? (1, 2, 4) : (1, 2, 4, 8);

    my $bench_dir = File::Spec->catdir($ROOT, 'tools', 'bench');
    my @drivers = (
        ['mt',       File::Spec->catfile($bench_dir, 'mt.pl'),       1],
        ['ithreads', File::Spec->catfile($bench_dir, 'ithreads.pl'), 1],
        ['prefork',  File::Spec->catfile($bench_dir, 'prefork.pl'),  1],
    );

    print "### 5a. Shared-Image Worker Pool vs `ithreads` vs `prefork`\n\n";
    print "Workload: `tools/bench/Workload.pm` with `--image-mb $img_mb --items $items` (median of $reps runs).\n\n";
    print "| Workers | Driver | Throughput (ops/s) | Total RSS (MiB) | RSS / Worker (MiB) | Startup (ms) |\n";
    print "|---:|---|---:|---:|---:|---:|\n";

    for my $w (@workers) {
        for my $drv (@drivers) {
            my ($label, $script, $fib) = @$drv;
            my (@ops, @rss, @rpw, @st);
            for (1 .. $reps) {
                my @args = ($script, '--workers', $w, '--items', $items, '--image-mb', $img_mb);
                push @args, ('--fibers', $fib) if $label eq 'mt';
                my $out = run_child_perl({}, @args);
                if ($out =~ /ops_per_s=([\d.]+)/) {
                    push @ops, $1;
                    push @rss, ($out =~ /rss_kb=(\d+)/ ? $1 : 0);
                    push @rpw, ($out =~ /rss_per_worker_kb=(\d+)/ ? $1 : 0);
                    push @st,  ($out =~ /startup_ms=([\d.]+)/ ? $1 : 0);
                }
            }
            printf "| %d | `%s` | %.1f | %.1f | %.2f | %.2f |\n",
                $w, $label,
                median(@ops),
                median(@rss) / 1024.0,
                median(@rpw) / 1024.0,
                median(@st);
        }
    }
    print "\n";

    print "### 5b. Cooperative Stackful Fibers (`MT::Pool` + 4 `MT::Fiber`s / Worker)\n\n";
    my @qflag = $quick ? ('--quick') : ();
    my $fout = run_child_perl({}, $0, '--child', 'suite5_fibers', @qflag);
    my ($sw_ns, $sw_ops, $sw_n) = (0, 0, 0);
    my @frows;
    for my $line (split /\n/, $fout) {
        ($sw_ns, $sw_ops, $sw_n) = ($1, $2, $3) if $line =~ /^FIBER_SWITCH ([\d.]+) ([\d.]+) (\d+)/;
        push @frows, [$1, $2, $3] if $line =~ /^FIBER_POOL (\d+) (\d+) ([\d.]+)/;
    }
    printf "Fiber `resume`+`yield` round-trip (%d switches): **%.1f ns** (**%.2f M switches/s**).\n\n",
        $sw_n, $sw_ns, $sw_ops / 1e6;
    print "| Workers | Fibers / Worker | Total Fibers | Throughput (items/s) | Scaling vs 1W |\n";
    print "|---:|---:|---:|---:|---:|\n";
    my $base_fops = @frows ? ($frows[0][2] || 1) : 1;
    for my $r (@frows) {
        my ($w, $nf, $ops) = @$r;
        printf "| %d | %d | %d | %.1f | **%.2fx** |\n",
            $w, $nf, $w * $nf, $ops, $ops / $base_fops;
    }
    print "\n";
}

# ---- Suite 6: Parallel Ndarray & Concurrency Latencies --------------------

if ($want{6}) {
    print "## Suite 6: Parallel Ndarray (`MT::PDLBridge`) & Concurrency Primitives\n\n";

    # 6a: tools/pdl-proof.pl
    my $nelem   = $quick ? 131_072 : 524_288;
    my $workers = 4;
    my $pdl_script = File::Spec->catfile($ROOT, 'tools', 'pdl-proof.pl');
    my $pdl_out = run_child_perl(
        { PDL_PROOF_NELEM => $nelem, PDL_PROOF_WORKERS => $workers },
        $pdl_script,
    );

    my ($sc_sha, $sc_sec, $wk_sha, $wk_sec, $parity) = ('', 0, '', 0, 'UNKNOWN');
    if ($pdl_out =~ /scalar_sha256\s*:\s*([0-9a-f]+)\s+\(([\d.]+)s\)/i) {
        ($sc_sha, $sc_sec) = ($1, $2);
    }
    if ($pdl_out =~ /worker_sha256\s*:\s*([0-9a-f]+)\s+\(([\d.]+)s\)/i) {
        ($wk_sha, $wk_sec) = ($1, $2);
    }
    if ($pdl_out =~ /digest_parity\s*:\s*(\S+)/) {
        $parity = $1;
    }
    my $pdl_speedup = $wk_sec > 0 ? $sc_sec / $wk_sec : 0;

    print "### 6a. Parallel Shared-Memory Ndarray (`MT::PDLBridge`, `nelem=$nelem`, `workers=$workers`)\n\n";
    print "| Mode | Elapsed (ms) | Speedup | SHA-256 Digest Parity |\n";
    print "|---|---:|---:|---|\n";
    printf "| 1-thread scalar reference | %.2f | 1.00x | `%s...` |\n",
        $sc_sec * 1000, substr($sc_sha, 0, 16);
    printf "| %d-worker shm `MT::Pool` | %.2f | **%.2fx** | **%s** (`%s...`) |\n\n",
        $workers, $wk_sec * 1000, $pdl_speedup, $parity, substr($wk_sha, 0, 16);

    # 6b: MT::Cell & MT::Channel
    my @qflag = $quick ? ('--quick') : ();
    my $conc_out = run_child_perl({}, $0, '--child', 'suite6_conc', @qflag);
    my ($cas_ops, $cas_us, $cas_n) = (0, 0, 0);
    my ($get_ops, $get_us, $get_n) = (0, 0, 0);
    my ($rtt_us,  $rtt_ops, $rtt_n) = (0, 0, 0);

    for my $line (split /\n/, $conc_out) {
        ($cas_ops, $cas_us, $cas_n) = ($1, $2, $3) if $line =~ /^CELL_CAS ([\d.]+) ([\d.]+) (\d+)/;
        ($get_ops, $get_us, $get_n) = ($1, $2, $3) if $line =~ /^CELL_GET ([\d.]+) ([\d.]+) (\d+)/;
        ($rtt_us,  $rtt_ops, $rtt_n) = ($1, $2, $3) if $line =~ /^CHAN_RTT ([\d.]+) ([\d.]+) (\d+)/;
    }

    print "### 6b. Lock-Free `MT::Cell` & Cross-Worker `MT::Channel` Latencies\n\n";
    print "| Primitive | Operation | Throughput | Mean Latency |\n";
    print "|---|---|---:|---:|\n";
    printf "| `MT::Cell` | Lock-free `cas()` (%d ops) | **%.2f Mops/s** | %.3f µs |\n",
        $cas_n, $cas_ops / 1e6, $cas_us;
    printf "| `MT::Cell` | Lock-free `get()` (%d ops) | **%.2f Mops/s** | %.3f µs |\n",
        $get_n, $get_ops / 1e6, $get_us;
    printf "| `MT::Channel` | Cross-worker ping-pong RTT (%d trips) | **%.0f trips/s** | **%.2f µs** |\n\n",
        $rtt_n, $rtt_ops, $rtt_us;
}

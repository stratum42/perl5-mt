#!./perl
use strict;
use warnings;
use Test::More;
use Config;
use Hash::Util qw(bucket_array bucket_ratio num_buckets lock_keys unlock_keys);
use Storable qw(freeze thaw);

# Child mode: run deterministic workload and print digest
if (@ARGV && $ARGV[0] eq '--child') {
    my $ops = $ARGV[1] || 200_000;
    my $max_keys = $ARGV[2] || 50_000;
    my $seed = 0x12345678;
    my $rng = sub {
        $seed = ($seed * 1103515245 + 12345) & 0x7fffffff;
        return $seed;
    };

    my %h;
    my $acc = 0;
    for my $i (1 .. $ops) {
        my $k = "k_" . ($rng->() % $max_keys);
        my $op = $rng->() & 7;
        if ($op <= 2) {
            $h{$k} = $i;
        } elsif ($op <= 4) {
            my $v = $h{$k};
            $acc = ($acc * 31 + (defined($v) ? $v : 0)) & 0x7fffffff;
        } elsif ($op == 5) {
            $acc = ($acc * 31 + (exists($h{$k}) ? 1 : 0)) & 0x7fffffff;
        } elsif ($op == 6) {
            my $v = delete $h{$k};
            $acc = ($acc * 31 + (defined($v) ? $v : 7)) & 0x7fffffff;
        } else {
            $acc = ($acc * 31 + scalar(%h)) & 0x7fffffff;
        }
    }

    my @sk = sort keys %h;
    my $kdig = 0;
    for my $k (@sk) {
        $kdig = ($kdig * 33 + length($k) + $h{$k}) & 0x7fffffff;
    }

    # Verify each visits every key exactly once
    my %seen;
    my $ecount = 0;
    while (my ($ek, $ev) = each %h) {
        die "duplicate key $ek in each" if $seen{$ek}++;
        die "value mismatch for $ek" if $ev != $h{$ek};
        $ecount++;
    }
    die "each count $ecount != scalar " . scalar(%h) if $ecount != scalar(%h);

    # Verify list context
    my @kv = %h;
    die "list context length mismatch" if @kv != 2 * scalar(%h);

    print "DIGEST $acc $kdig " . scalar(%h) . "\n";
    exit 0;
}

# 1. Basic split across PERL_HASH_DEFAULT_HvMAX (7 -> 16 -> ... -> 65536)
{
    my %h;
    for my $i (1 .. 5000) {
        $h{"key_$i"} = $i * 3;
    }
    is(scalar(%h), 5000, "5000 keys stored");
    my $ok = 1;
    for my $i (1 .. 5000) {
        if (!exists $h{"key_$i"} || $h{"key_$i"} != $i * 3) {
            $ok = 0;
            last;
        }
    }
    ok($ok, "all 5000 keys fetched accurately");
}

# 2. UTF-8, Latin-1 upgrade/downgrade, and embedded NUL keys
{
    my %h;
    for my $i (1 .. 64) {
        $h{"pad_$i"} = $i;
    }
    my $lat = "\x80\xff";
    my $u8  = "\x80\xff";
    utf8::upgrade($u8);
    $h{$lat} = "latin1";
    is($h{$u8}, "latin1", "UTF-8 upgraded lookup matches Latin-1 key");
    $h{$u8} = "upgraded";
    is($h{$lat}, "upgraded", "Latin-1 lookup matches updated UTF-8 key");

    my $wide = "\x{263a}_smile";
    $h{$wide} = 42;
    ok(exists $h{$wide}, "wide UTF-8 key exists");
    is($h{$wide}, 42, "wide UTF-8 key value");

    $h{"a\0b"} = 101;
    $h{"a\0c"} = 102;
    is($h{"a\0b"}, 101, "embedded NUL key 1");
    is($h{"a\0c"}, 102, "embedded NUL key 2");
    is(delete $h{"a\0b"}, 101, "delete embedded NUL key");
    ok(!exists $h{"a\0b"}, "deleted NUL key gone");
    is($h{"a\0c"}, 102, "sibling NUL key intact");
}

# 3. each() and delete during each()
{
    my %h = map { ("k$_" => $_) } 1 .. 500;
    my %seen;
    while (my ($k, $v) = each %h) {
        $seen{$k} = $v;
        if ($v % 2 == 0) {
            delete $h{$k};
        }
    }
    is(scalar(keys %seen), 500, "each visited all 500 keys once while deleting evens");
    is(scalar(%h), 250, "250 odd keys remain after delete-during-each");
    my $odds_ok = 1;
    for my $i (1 .. 500) {
        if ($i % 2 == 0) {
            $odds_ok = 0 if exists $h{"k$i"};
        } else {
            $odds_ok = 0 if !exists $h{"k$i"} || $h{"k$i"} != $i;
        }
    }
    ok($odds_ok, "exact odd keys survive delete-during-each");
}

# 4. Hash::Util bucket_array / bucket_ratio / num_buckets / lock_keys (placeholders)
{
    my %h = map { ("bu_$_" => $_) } 1 .. 200;
    my $ba = bucket_array(\%h);
    my $nb = num_buckets(%h);
    ok($nb >= 256, "num_buckets ($nb) >= 256 for 200 keys");
    my $sum_counts = 0;
    my $non_empty = 0;
    my $total_buckets = 0;
    for my $e (@$ba) {
        if (ref($e) eq 'ARRAY') {
            $sum_counts += scalar(@$e);
            $non_empty++;
            $total_buckets++;
        } else {
            $total_buckets += $e;
        }
    }
    is($total_buckets, $nb, "bucket_array total buckets matches num_buckets");
    is($sum_counts, 200, "sum of bucket_array keys equals key count");
    my $ratio = bucket_ratio(%h);
    is($ratio, "$non_empty/$nb", "bucket_ratio matches bucket_array non-empty/total");

    # Restricted hash with placeholders
    lock_keys(%h);
    delete $h{"bu_10"};
    ok(!exists $h{"bu_10"}, "deleted key in restricted Swiss hash does not exist");
    is(scalar(%h), 199, "scalar(%h) excludes placeholder");
    $h{"bu_10"} = 999;
    is($h{"bu_10"}, 999, "can reassign placeholder key in restricted Swiss hash");
    eval { $h{"disallowed_key"} = 1 };
    like($@, qr/disallowed key/i, "disallowed key croaks in restricted Swiss hash");
    unlock_keys(%h);
    $h{"allowed_now"} = 777;
    is($h{"allowed_now"}, 777, "unlocked Swiss hash accepts new keys");
}

# 5. Storable freeze/thaw round trip
{
    my %h = map { ("st_$_" => { idx => $_, arr => [1, $_] }) } 1 .. 150;
    my $frozen = freeze(\%h);
    my $thawed = thaw($frozen);
    is(scalar(keys %$thawed), 150, "Storable round trip key count");
    is($thawed->{st_42}{idx}, 42, "Storable round trip nested value");
    is($thawed->{st_150}{arr}[1], 150, "Storable round trip nested array");
}

# 6. %h = (), undef %h, and re-use
{
    my %h = map { ("cl_$_" => $_) } 1 .. 300;
    %h = ();
    is(scalar(%h), 0, "%h = () empties Swiss hash");
    is(scalar(keys %h), 0, "keys %h empty after clear");
    for my $i (1 .. 300) { $h{"re_$i"} = $i }
    is(scalar(%h), 300, "re-populated after %h = ()");
    undef %h;
    is(scalar(%h), 0, "undef %h empties Swiss hash");
    for my $i (1 .. 100) { $h{"au_$i"} = $i }
    is(scalar(%h), 100, "re-populated after undef %h");
}

# 7. Blessed values with DESTROY (including re-entrant mutation)
{
    my $destroyed = 0;
    {
        package SwissDest;
        sub new { bless { id => $_[1], cb => $_[2] }, $_[0] }
        sub DESTROY {
            my $self = shift;
            $destroyed++;
            $self->{cb}->() if $self->{cb};
        }
    }
    my %h;
    for my $i (1 .. 100) {
        $h{"obj_$i"} = SwissDest->new($i, $i == 50 ? sub { $h{extra_from_destroy} = 1 } : undef);
    }
    %h = ();
    is($destroyed, 100, "all 100 DESTROY methods called on Swiss hash clear");
}

# 8. Large-scale 100k keys insert/hit/miss/delete/iterate + tombstone churn
{
    my %h;
    for my $i (1 .. 100_000) {
        $h{"big_$i"} = $i;
    }
    is(scalar(%h), 100_000, "100k keys inserted");
    # Delete 80% and re-insert to exercise tombstone compaction rehash
    for my $i (1 .. 80_000) {
        delete $h{"big_$i"};
    }
    is(scalar(%h), 20_000, "20k keys remain after 80k deletes");
    for my $i (100_001 .. 180_000) {
        $h{"big_$i"} = $i;
    }
    is(scalar(%h), 100_000, "100k keys after tombstone churn");
    my $sum = 0;
    while (my ($k, $v) = each %h) {
        $sum += $v;
    }
    my $expected = 0;
    $expected += $_ for 80_001 .. 180_000;
    is($sum, $expected, "100k each() sum exact after churn");
}

# 9. Cross-mode digest parity: Swiss SIMD vs Swiss SWAR (PERL_FORCE_SCALAR=1) vs Chained (PERL_MT_SWISS=0)
{
    my $ops = $ENV{PERL_SWISS_OPS} || 250_000;
    my $maxk = 50_000;
    my $run = sub {
        my (%env) = @_;
        local %ENV = (%ENV, %env);
        my $inc = -f 'lib/strict.pm' ? '-I. -Ilib' : '-I.. -I../lib';
        my $out = `$^X $inc $0 --child $ops $maxk`;
        die "child failed ($?): $out" if $? != 0;
        chomp $out;
        return $out;
    };

    my $d_swiss_simd   = $run->(PERL_MT_SWISS => 1, PERL_FORCE_SCALAR => 0);
    my $d_swiss_scalar = $run->(PERL_MT_SWISS => 1, PERL_FORCE_SCALAR => 1);
    my $d_chained      = $run->(PERL_MT_SWISS => 0, PERL_FORCE_SCALAR => 0);
    my $d_p0           = $run->(PERL_MT_SWISS => 1, PERL_PERTURB_KEYS => 0);
    my $d_p2           = $run->(PERL_MT_SWISS => 1, PERL_PERTURB_KEYS => 2);

    like($d_swiss_simd, qr/^DIGEST \d+ \d+ \d+$/, "Swiss SIMD digest valid: $d_swiss_simd");
    is($d_swiss_scalar, $d_swiss_simd, "Swiss SWAR scalar matches Swiss SIMD");
    is($d_chained,      $d_swiss_simd, "Chained (PERL_MT_SWISS=0) matches Swiss SIMD");
    is($d_p0,           $d_swiss_simd, "PERL_PERTURB_KEYS=0 matches digest");
    is($d_p2,           $d_swiss_simd, "PERL_PERTURB_KEYS=2 matches digest");
}

done_testing();

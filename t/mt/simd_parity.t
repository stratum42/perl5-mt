#!./perl
# perl5-mt: the SIMD kernels against the scalar twins.
#
# The parent writes one fixed corpus (RFC 3629 boundaries, overlongs,
# surrogates, code points above 0x10FFFF, Perl's extended sequences, truncated
# sequences, Latin-1 mixes, pure ASCII of every length 0..300, and seeded
# random mixes with high-bit runs at random offsets) to a temp file.  Child
# perls read it and run one fixed battery over every string, printing the
# level they run at and one digest of every result.  The scalar child runs the
# upstream code (PERL_FORCE_SCALAR=1); the default child runs the SIMD hooks;
# a third child caps the level at the 16-byte bodies (PERL_SIMD_LEVEL=narrow).
# The digests must agree.  Run the test itself with PERL_FORCE_SCALAR set or
# unset: the children set their own mode, so the result must not change.
#
# PERL_MT_LONG_TESTS=1 makes the random part of the corpus larger.

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib', '../ext/MT/lib');
}

use strict;
use warnings;

my $long = $ENV{PERL_MT_LONG_TESTS};
plan(tests => 6);

# ---- the corpus: fixed seed, written once by the parent --------------------

my $seed = 0x5a5a1234;
sub rnd {
    my $n = shift;
    $seed = ($seed * 1103515245 + 12345) % 2147483648;
    return int($seed / 65536) % $n;
}

# Encode a code point with the standard UTF-8 pattern, extended to 6 bytes
# for values above 0x1FFFFF (Perl's extended form)
sub enc {
    my $cp = shift;
    return chr($cp) if $cp < 0x80;
    my @b;
    my $n = $cp < 0x800 ? 2 : $cp < 0x10000 ? 3 : $cp < 0x200000 ? 4
          : $cp < 0x4000000 ? 5 : 6;
    my $lead = (0xFF << (8 - $n)) & 0xFF;
    for (my $i = $n - 1; $i > 0; $i--) {
        unshift @b, 0x80 | ($cp & 0x3F);
        $cp >>= 6;
    }
    unshift @b, $lead | $cp;
    return join '', map { chr } @b;
}

my @strings;
my $pad = sub { ("x" x $_[0]) };

# Boundaries and specials, each alone, padded, and as a run of 40
for my $cp (0x7F, 0x80, 0x7FF, 0x800, 0xD7FF, 0xD800, 0xDFFF, 0xE000,
            0xFFFD, 0xFFFE, 0xFFFF, 0x10000, 0x10FFFF, 0x110000,
            0x1FFFFF, 0x200000, 0x3FFFFFF, 0x4000000, 0x7FFFFFFF) {
    my $c = enc($cp);
    push @strings, $c, $pad->(70) . $c . $pad->(70), $c x 40;
}

# Overlongs, truncated and stray bytes, bad leads
for my $bad ("\xC0\x80", "\xC1\xBF", "\xE0\x80\x80", "\xE0\x9F\xBF",
             "\xF0\x80\x80\x80", "\xF0\x8F\xBF\xBF", "\xF8\x80\x80\x80\x80",
             "\xFC\x80\x80\x80\x80\x80", "\xED\xA0\x80", "\xED\xBF\xBF",
             "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xFF", "\x80", "\xBF",
             "\xE2\x82", "\xF0\x9F\x98", "\xC3", "\xE2", "\xF4\x8F\xBF\xBF",
             "\xEE\x80\x80", "\xEF\xBF\xBF", "\xF0\x90\x80\x80") {
    push @strings, $bad, $pad->(70) . $bad, $pad->(70) . $bad . $pad->(70),
                   $pad->(90) . $bad;
}

# Pure ASCII of every length 0..300
for my $len (0 .. 300) {
    push @strings, join '', map { chr(0x20 + ($_ % 90)) } 1 .. $len;
}

# Latin-1 mixes: high bytes at random offsets in ASCII
for (1 .. 60) {
    my $len = 1 + rnd(400);
    my $s = '';
    while (length($s) < $len) {
        if (rnd(3)) {
            $s .= chr(0x20 + rnd(90)) x (1 + rnd(40));
        }
        else {
            $s .= join '', map { chr(0x80 + rnd(128)) } 1 .. (1 + rnd(20));
        }
    }
    push @strings, substr($s, 0, $len);
}

# Seeded random mixes of valid and invalid sequences, up to 100000 bytes
my $count = $long ? 60 : 20;
my $maxlen = $long ? 100_000 : 20_000;
for (1 .. $count) {
    my $target = 1 + rnd($maxlen);
    my $s = '';
    while (length($s) < $target) {
        my $k = rnd(10);
        if ($k < 4) {
            $s .= chr(0x20 + rnd(90)) x (1 + rnd(200));
        }
        elsif ($k < 6) {
            $s .= enc(0x80 + rnd(0x700)) x (1 + rnd(30));
        }
        elsif ($k < 7) {
            $s .= enc(0x800 + rnd(0xF000)) x (1 + rnd(30));
        }
        elsif ($k < 8) {
            $s .= enc(0x10000 + rnd(0x100000)) x (1 + rnd(10));
        }
        elsif ($k < 9) {
            $s .= join '', map { chr(0x80 + rnd(128)) } 1 .. (1 + rnd(100));
        }
        else {
            $s .= (rnd(2) ? "\xE2\x82" : "\xF0\x9F") . chr(0x20 + rnd(90));
        }
    }
    push @strings, substr($s, 0, $target);
}

# Valid UTF-8 of every length, from ASCII runs and characters of 2, 3 and 4
# bytes at their boundaries; then corrupted and truncated copies of some
my @valid_chars = (0x80, 0x7FF, 0x800, 0xD7FF, 0xE000, 0xFFFD, 0xFFFF,
                   0x10000, 0x10FFFF, 0xE9, 0x20AC, 0x1F600);
my $valid_count = $long ? 60 : 20;
my $valid_max = $long ? 100_000 : 20_000;
for (1 .. $valid_count) {
    my $target = 1 + rnd($valid_max);
    my $s = '';
    while (length($s) < $target) {
        if (rnd(3)) {
            $s .= chr(0x20 + rnd(90)) x (1 + rnd(120));
        }
        else {
            $s .= enc($valid_chars[rnd(scalar @valid_chars)]);
        }
    }
    $s = substr($s, 0, $target) if length($s) > $target;
    # keep only complete characters: back off a cut sequence
    utf8::decode(my $probe = $s) or do {
        $s =~ s/[\x80-\xFF]+\z//;
    };
    push @strings, $s;
    my $bad = $s;
    substr($bad, rnd(length($bad)), 1) = chr(0x80 + rnd(128)) if length $bad;
    push @strings, $bad;
    push @strings, substr($s, 0, length($s) - 1 - rnd(3)) if length($s) > 4;
}

my $corpus_bytes = join '', map { pack("N/a*", $_) } @strings;
my $corpus = tempfile();
open my $cfh, ">:raw", $corpus or die "open $corpus: $!";
print $cfh $corpus_bytes or die "write $corpus: $!";
close $cfh or die "close $corpus: $!";

ok(@strings > 500 && length($corpus_bytes) > 0,
   "corpus written: " . scalar(@strings) . " strings, " .
   length($corpus_bytes) . " bytes");

# ---- the battery, run by each child -----------------------------------------

my $battery = <<'BATTERY';
use strict; use warnings; $| = 1;
use Digest::MD5 qw(md5_hex);

# the digest of a string that may be wide: its UTF-8 encoding
sub mh { my $x = shift; utf8::encode($x) if utf8::is_utf8($x); return md5_hex($x) }
my $have_api = eval { require XS::APItest; 1 } ? 1 : 0;

# tr with a random 1:1 map of all 256 bytes, compiled once
my @perm = (0 .. 255);
my $pseed = 0x2545F491;
for my $i (reverse 1 .. 255) {
    $pseed = ($pseed * 1103515245 + 12345) % 2147483648;
    my $j = $pseed % ($i + 1);
    @perm[$i, $j] = @perm[$j, $i];
}
my $perm_to = join '', map { sprintf '\\x%02x', $_ } @perm;
my $perm_from = join '', map { sprintf '\\x%02x', $_ } 0 .. 255;
my $tr_perm = eval "sub { my \$x = shift; \$x =~ tr/$perm_from/$perm_to/; \$x }"
    or die "tr map: $@";
my ($file) = @ARGV;
open my $fh, '<:raw', $file or die "open $file: $!";
my $all = do { local $/; <$fh> };
close $fh;
my $ctx = Digest::MD5->new;
my $n = 0;
my $pos = 0;
while ($pos < length $all) {
    my $len = unpack 'N', substr($all, $pos, 4);
    my $s = substr($all, $pos + 4, $len);
    $pos += 4 + $len;
    $n++;

    my $l = length $s;

    my $u = $s;
    utf8::upgrade($u);
    my $ul = length $u;
    my $uv = utf8::valid($u) ? 1 : 0;

    my $d = $s;
    my $dok = utf8::decode($d) ? 1 : 0;
    my $dl = length $d;

    my $sv = utf8::valid($s) ? 1 : 0;

    my @sub;
    for my $k (0, 1, int($dl / 3), int($dl / 2), $dl - 1) {
        push @sub, ord(substr($d, $k, 1)) if $k >= 0 && $k < $dl;
    }
    my @usub;
    for my $k (0, 1, int($ul / 2), $ul - 1) {
        push @usub, ord(substr($u, $k, 1)) if $k >= 0 && $k < $ul;
    }

    my @lf;
    if ($have_api) {
        for my $f (0, 0x4000, 0x100) {
            push @lf, join('/', @{ XS::APItest::test_is_utf8_string_loclen_flags($s, length $s, $f) });
        }
        push @lf, join('/', @{ XS::APItest::test_is_utf8_string_loclen_flags($d, length $d, 0) });
    }

    $ctx->add(join(' ', $l, $ul, $uv, $dok, $dl, $sv, "s[@sub]", "u[@usub]", "f[@lf]"), "\n");

    # index and rindex with needles of 1..40 bytes, from the string and absent
    my $rs = ($n * 2654435761) % 4294967296;
    for my $m (1, 2, 3, 4, 7, 15, 31, 32, 33, 40) {
        next if $l < $m;
        $rs = ($rs * 1103515245 + 12345) % 2147483648;
        my $at = $rs % ($l - $m + 1);
        my $needle = substr($s, $at, $m);
        $ctx->add(join(' ', 'ix', $m, index($s, $needle), rindex($s, $needle),
                       index($s, $needle, $at + 1), index($s, "\x01" x $m)), "\n");
    }

    # tr: a range map, a shifted range, a three-byte map, a random permutation
    my $t1 = $s; my $n1 = ($t1 =~ tr/a-z/A-Z/);
    my $t2 = $s; my $n2 = ($t2 =~ tr/a-y/b-z/);
    my $t3 = $s; my $n3 = ($t3 =~ tr/abc/xyz/);
    my $t4 = $tr_perm->($s);
    $ctx->add(join(' ', 'tr', $n1, md5_hex($t1), $n2, md5_hex($t2), $n3,
                   md5_hex($t3), md5_hex($t4)), "\n");

    # uc and lc of the bytes, and of the upgraded string
    $ctx->add(join(' ', 'uc', md5_hex(uc $s), md5_hex(lc $s),
                   mh(uc $u), mh(lc $u)), "\n");

    # split on a one-byte separator, with and without a limit, and on x
    my @f = split /,/, $s;
    my @g = split /,/, $s, 3;
    my @h = split /,/, $s, -1;
    my @x = split /x/, $s;
    $ctx->add(join(' ', 'sp', scalar(@f), md5_hex(join("\x1f", @f)),
                   scalar(@g), md5_hex(join("\x1f", @g)),
                   scalar(@h), md5_hex(join("\x1f", @h)),
                   scalar(@x), md5_hex(join("\x1f", @x))), "\n");

# regex span kernels: matches and captures of runs and classes, on
# the byte string and on its UTF-8 copy (which takes the upstream path)
my @rx = (
    qr/x*/,
    qr/([^,]+)/,
    qr/([a-f0-9]+)/,
    qr/([^\x00-\x7F]+)/,
    qr/(\d{3,})/,
    qr/([aeiou]*z)/,
    qr/([a-z]+)/i,
    qr/([a-f0-9]+)/i,
    qr/([k-p]+)/,
    qr/(\s*[^\s,;]+)/,
);
for my $pass ([$s, 'b'], [$u, 'u']) {
    my ($str, $tag) = @$pass;
    for my $i (0 .. $#rx) {
        my @hits;
        while ($str =~ /$rx[$i]/g) {
            push @hits, $-[0], $+[0], defined $1 ? $1 : '';
        }
        $ctx->add(join(' ', 'rx', $tag, $i, scalar(@hits),
                       md5_hex(join("\x1f", @hits))), "\n");
    }
}

# s///g with classes, and split on a class of two bytes
my $sg1 = $s; my $ng1 = ($sg1 =~ s/[^,]+/x/g);
my $sg2 = $s; my $ng2 = ($sg2 =~ s/(\d{3,})/<$1>/g);
my @sc = split /[;,]/, $s, -1;
$ctx->add(join(' ', 'sg', $ng1, md5_hex($sg1), $ng2, md5_hex($sg2),
               scalar(@sc), md5_hex(join("\x1f", @sc))), "\n");
}
print "level=", MT::SIMD::level_name(), "\n";
print "apitest=$have_api\n";
print "strings=$n\n";
print "digest=", $ctx->hexdigest, "\n";
BATTERY

sub child {
    my ($env) = @_;
    my $file = tempfile();
    open my $fh, ">", $file or die "open $file: $!";
    print $fh $battery;
    close $fh or die "close $file: $!";

    my %res;
    {
        local %ENV = (%ENV, %$env);
        for my $k (qw(PERL_FORCE_SCALAR PERL_SIMD_LEVEL)) {
            delete $ENV{$k} unless exists $env->{$k};
        }
        my $out = runperl(progfile => $file, args => [$corpus], stderr => 1,
                      switches => ['-I../lib']);
        $out =~ s/\r\n/\n/g;
        for my $line (split /\n/, $out) {
            $res{$1} = $2 if $line =~ /^(level|strings|digest)=(.*)$/;
        }
        $res{raw} = $out;
    }
    return \%res;
}

my $scalar = child({ PERL_FORCE_SCALAR => 1 });
my $simd   = child({});
my $narrow = child({ PERL_SIMD_LEVEL => 'narrow' });

is($scalar->{level}, 'scalar', 'the forced child runs the scalar level')
    or diag($scalar->{raw});

ok($simd->{level} && $simd->{level} ne 'scalar',
   "the default child runs a SIMD level ($simd->{level})")
    or diag($simd->{raw});

is($simd->{digest}, $scalar->{digest},
   'SIMD hooks give the scalar results on every corpus string')
    or diag($simd->{raw}, $scalar->{raw});

ok($narrow->{level} && $narrow->{level} ne 'avx2',
   "the narrow child stops at a 16-byte level ($narrow->{level})")
    or diag($narrow->{raw});

is($narrow->{digest}, $scalar->{digest},
   'the 16-byte bodies give the scalar results on every corpus string')
    or diag($narrow->{raw}, $scalar->{raw});

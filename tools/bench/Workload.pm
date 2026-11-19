package Workload;

# perl5-mt: reworked benchmark workload (tools/bench).
#
# Supports:
#   - Large preloaded image (PERL_BENCH_IMAGE_MB, e.g. 256 MiB) before freeze
#     or fork/ithreads clone, so RSS per added worker reflects real code+data
#     footprints rather than empty interpreter overhead.
#   - In-process (PERL_XS_THREADSAFE) and out-of-process (compartment) JSON
#     rows via Cpanel::JSON::XS, with automatic JSON::PP fallback when absent.
#   - Physical core detection on macOS, Linux, and AIX for SMT-aware sweeps.
#   - One work item: 16 KiB file read (:async under perl5-mt, :unix otherwise),
#     UNIX socketpair echo, 10 KiB JSON round trip, and a pure-Perl FNV-1a loop.

use strict;
use warnings;
use Exporter 'import';
use Socket qw(AF_UNIX SOCK_STREAM PF_UNSPEC);
use File::Temp ();

our @EXPORT_OK = qw(
    setup item layer_name json_mode_name image_mb
    rss_kb rss_per_worker_kb physical_cores preload_image
);

our ($FILE, $DOC, $JSON, $LAYER, $JSON_MODE, $IMAGE_MB);
our @PRELOADED_CHUNKS;

sub physical_cores {
    if ($^O eq 'darwin') {
        my $n = `sysctl -n hw.physicalcpu 2>/dev/null`;
        return $1 if defined $n && $n =~ /(\d+)/ && $1 > 0;
    }
    elsif ($^O eq 'linux') {
        my $out = `lscpu -p=CORE,SOCKET 2>/dev/null`;
        if (defined $out && length $out) {
            my %seen;
            for my $line (split /\n/, $out) {
                next if $line =~ /^#/ || $line !~ /^\d+,\d+/;
                $seen{$line} = 1;
            }
            my $n = scalar keys %seen;
            return $n if $n > 0;
        }
        my $n = `nproc 2>/dev/null`;
        return $1 if defined $n && $n =~ /(\d+)/ && $1 > 0;
    }
    elsif ($^O eq 'aix') {
        my $out = `lparstat -i 2>/dev/null`;
        if (defined $out && $out =~ /Online Virtual CPUs\s*:\s*(\d+)/) {
            return $1 if $1 > 0;
        }
    }
    return 4;
}

# Build a preloaded read-only image of roughly $target_mb MiB before freeze
# (or before fork / threads->create) using realistic preloaded packages,
# subs, and read-only lookup tables.
sub preload_image {
    my ($target_mb) = @_;
    $target_mb = 0 unless defined $target_mb && $target_mb > 0;
    $IMAGE_MB = $target_mb;
    return 0 unless $target_mb > 0;

    my $chunk_kb = 256;
    my $nchunks  = int(($target_mb * 1024) / $chunk_kb);
    $nchunks = 1 if $nchunks < 1;

    # Populate preloaded packages and immutable lookup data
    my $nmods = $nchunks > 64 ? 64 : $nchunks;
    for my $m (0 .. $nmods - 1) {
        my $pkg = "Bench::Preload::Mod$m";
        eval qq{
            package $pkg;
            sub id { $m }
            sub mix { ((\$_[0] // 0) * 2654435761 + $m) & 0xffffffff }
            1;
        } or die $@;
    }

    my $seed = join('', ('a'..'z', 'A'..'Z', '0'..'9', '_', '-')) x 4096; # 256 KiB
    for my $c (0 .. $nchunks - 1) {
        my $buf = substr($seed, 0, $chunk_kb * 1024);
        substr($buf, 0, 12, sprintf("BLK%08d:", $c));
        push @PRELOADED_CHUNKS, $buf;
    }
    return $target_mb;
}

# Setup the workload once in the parent before freeze / fork / threads->create.
sub setup {
    my ($dir, %opt) = @_;
    $LAYER = (defined &MT::IO::stat && !$INC{'threads.pm'}) ? ':async' : ':unix';

    my $img_mb = defined $opt{image_mb}
        ? $opt{image_mb}
        : (defined $ENV{PERL_BENCH_IMAGE_MB} ? $ENV{PERL_BENCH_IMAGE_MB} : 0);
    preload_image($img_mb) if $img_mb > 0;

    my $tmp = File::Temp->new(DIR => $dir, UNLINK => 0);
    $FILE = $tmp->filename;
    close $tmp;
    open my $w, '>:unix', $FILE or die "write $FILE: $!";
    print $w "x" x 16384 or die "write: $!";
    close $w or die "close: $!";

    $DOC = {
        name  => 'document',
        items => [ map { { id => $_, label => "item $_", tags => [ 'a' .. 'e' ] } } 1 .. 120 ],
    };

    my $want_mode = $opt{json_mode} || $ENV{PERL_BENCH_JSON_MODE} || 'auto';
    if ($want_mode eq 'compartment') {
        # Defer loading until workers run so Cpanel::JSON::XS routes through
        # the out-of-process compartment when not marked threadsafe.
        $JSON_MODE = 'compartment';
        $JSON = undef;
    }
    else {
        my $loaded = eval {
            require Cpanel::JSON::XS;
            $JSON = Cpanel::JSON::XS->new->canonical;
            1;
        };
        if ($loaded) {
            if (defined &MT::XS::is_threadsafe && MT::XS::is_threadsafe('Cpanel::JSON::XS::encode')) {
                $JSON_MODE = 'inprocess_ts';
            }
            else {
                $JSON_MODE = 'inprocess';
            }
        }
        else {
            $JSON = undef;
            $JSON_MODE = 'pureperl';
        }
    }
    return $FILE;
}

sub _pp_encode {
    my ($v) = @_;
    return 'null' unless defined $v;
    my $r = ref $v;
    if ($r eq 'HASH') {
        return '{' . join(',', map {
            my $k = $_; $k =~ s/([\\"])/\\$1/g;
            '"' . $k . '":' . _pp_encode($v->{$_})
        } sort keys %$v) . '}';
    }
    elsif ($r eq 'ARRAY') {
        return '[' . join(',', map { _pp_encode($_) } @$v) . ']';
    }
    elsif ($v =~ /\A-?(?:0|[1-9]\d*)(?:\.\d+)?\z/) {
        return $v;
    }
    else {
        my $s = "$v";
        $s =~ s/([\\"])/\\$1/g;
        return '"' . $s . '"';
    }
}

sub _pp_decode_val {
    my ($s, $pos_ref) = @_;
    pos($$s) = $$pos_ref;
    $$s =~ /\G\s*/gc;
    if ($$s =~ /\G\{/gc) {
        my %h;
        $$s =~ /\G\s*/gc;
        if ($$s !~ /\G\}/gc) {
            while (1) {
                $$s =~ /\G\s*"((?:[^"\\]|\\.)*)"\s*:\s*/gc or die "bad json key";
                my $k = $1;
                $$pos_ref = pos($$s);
                $h{$k} = _pp_decode_val($s, $pos_ref);
                pos($$s) = $$pos_ref;
                $$s =~ /\G\s*/gc;
                last if $$s =~ /\G\}/gc;
                $$s =~ /\G,/gc or die "expected , or }";
            }
        }
        $$pos_ref = pos($$s);
        return \%h;
    }
    elsif ($$s =~ /\G\[/gc) {
        my @a;
        $$s =~ /\G\s*/gc;
        if ($$s !~ /\G\]/gc) {
            while (1) {
                $$pos_ref = pos($$s);
                push @a, _pp_decode_val($s, $pos_ref);
                pos($$s) = $$pos_ref;
                $$s =~ /\G\s*/gc;
                last if $$s =~ /\G\]/gc;
                $$s =~ /\G,/gc or die "expected , or ]";
            }
        }
        $$pos_ref = pos($$s);
        return \@a;
    }
    elsif ($$s =~ /\G"((?:[^"\\]|\\.)*)"/gc) {
        my $v = $1;
        $$pos_ref = pos($$s);
        return $v;
    }
    elsif ($$s =~ /\G(-?(?:0|[1-9]\d*)(?:\.\d+)?)/gc) {
        my $v = 0 + $1;
        $$pos_ref = pos($$s);
        return $v;
    }
    elsif ($$s =~ /\Gnull/gc) {
        $$pos_ref = pos($$s);
        return undef;
    }
    die "bad json at pos " . pos($$s);
}

sub _pp_decode {
    my ($text) = @_;
    my $pos = 0;
    return _pp_decode_val(\$text, \$pos);
}

sub _make_doc {
    return {
        name  => 'document',
        items => [ map { { id => $_, label => "item $_", tags => [ 'a' .. 'e' ] } } 1 .. 120 ],
    };
}

sub _get_doc {
    return _make_doc() if ($JSON_MODE // '') eq 'pureperl';
    return $DOC //= _make_doc();
}

sub layer_name     { $LAYER }
sub json_mode_name { $JSON_MODE // 'unknown' }
sub image_mb       { $IMAGE_MB // 0 }

sub _get_json {
    return $JSON if defined $JSON;
    if (($JSON_MODE // '') eq 'pureperl') {
        return undef;
    }
    my $ok = eval {
        require Cpanel::JSON::XS;
        1;
    };
    if ($ok) {
        return Cpanel::JSON::XS->new->canonical;
    }
    return undef;
}

# One item of work, numbered $i. Returns a deterministic checksum.
sub item {
    my ($i) = @_;
    my $sum = 0;

    # 1. File read: 16 KiB
    open my $fh, "<$LAYER", $FILE or die "open $FILE: $!";
    binmode $fh;
    local $/;
    my $data = <$fh>;
    close $fh;
    $sum += length $data;

    # 2. Socketpair echo
    socketpair(my $a, my $b, AF_UNIX, SOCK_STREAM, PF_UNSPEC) or die "socketpair: $!";
    my $msg = "ping $i " . ("y" x 48);
    syswrite($a, $msg) == length $msg or die "write: $!";
    sysread($b, my $back, length $msg) == length $msg or die "read: $!";
    close $a; close $b;
    $sum += length $back;

    # 3. JSON round trip
    my $codec = _get_json();
    my $doc   = _get_doc();
    my ($text, $round);
    if (defined $codec) {
        $text  = $codec->encode($doc);
        $round = $codec->decode($text);
    }
    else {
        $text  = _pp_encode($doc);
        $round = _pp_decode($text);
    }
    $sum += scalar @{ $round->{items} };

    # 4. Pure-Perl hashing loop (touching preloaded image if present)
    my $h = 2166136261;
    if (@PRELOADED_CHUNKS) {
        my $blk = $PRELOADED_CHUNKS[$i % scalar(@PRELOADED_CHUNKS)];
        $h ^= ord(substr($blk, $i & 255, 1));
    }
    for my $k (1 .. 1500) {
        $h ^= ($k * ($i + 1)) & 0xffff;
        $h = ($h * 16777619) & 0xffffffff;
    }
    $sum += $h & 0xff;
    return $sum;
}

# Resident set size in KiB across the given PIDs (portable on Linux, macOS, AIX).
sub rss_kb {
    my @pids = grep { defined && /^\d+$/ } @_;
    return 0 unless @pids;
    my $out = `ps -o rss= -p @{[ join ',', @pids ]} 2>/dev/null`;
    my $total = 0;
    $total += $_ for map { /(\d+)/ ? $1 : () } split /\n/, $out;
    return $total;
}

sub rss_per_worker_kb {
    my ($rss_total, $rss_base, $workers) = @_;
    return 0 unless $workers && $workers > 0;
    my $delta = $rss_total - $rss_base;
    $delta = 0 if $delta < 0;
    return int($delta / $workers);
}

1;

package MT::Checkpoint;

use strict;
use warnings;

BEGIN {
    require MT unless defined &MT::Pool::_encode_list;
}
use MT::SharedHash;

# perl5-mt: explicit data-only checkpoints for in-process process pairs
# and standalone in-memory or file-backed state snapshots.
#
# In-memory / pair checkpoints marshal $state into a capability (MT::Cap::mint)
# and record the capability handle in the pair's shared checkpoint store so the
# designated backup worker can immediately restore it upon primary fault.
# File-backed checkpoints write an atomic temp-file + rename snapshot with
# header magic and checksum verification.

our %LOCAL_MEM;      # fallback standalone in-memory store (name => cap/blob)
our $CTX;            # active pair context in a worker: { pair_id, store, is_backup, seq }

use constant FILE_MAGIC => "MTCK\x01";

sub _fnv1a_32 {
    my ($data) = @_;
    my $h = 2166136261;
    for my $b (unpack 'C*', $data) {
        $h ^= $b;
        $h = ($h * 16777619) & 0xffffffff;
    }
    return $h;
}

# Internal: called by MT::Pool primary and backup workers to bind the active
# job's checkpoint store.
sub _set_context {
    my (%ctx) = @_;
    $CTX = \%ctx;
    return 1;
}

sub _clear_context {
    $CTX = undef;
    return 1;
}

sub is_backup {
    return ($CTX && $CTX->{is_backup}) ? 1 : 0;
}

sub seq {
    return 0 unless $CTX;
    if ($CTX->{store} && defined $CTX->{pair_id}) {
        my $s = $CTX->{store}->get("p:$CTX->{pair_id}:seq");
        return defined $s ? $s : 0;
    }
    return $CTX->{seq} // 0;
}

sub _marshal {
    my ($state) = @_;
    return MT::Pool::_encode_list($state);
}

sub _unmarshal {
    my ($blob) = @_;
    return undef unless defined $blob && length $blob;
    my ($state) = MT::Pool::_decode_list($blob);
    return $state;
}

# Save a data-only checkpoint:
#   MT::Checkpoint::save($state)                  # active pair or in-memory default
#   MT::Checkpoint::save($state, name => 'k')     # named in-memory slot
#   MT::Checkpoint::save($state, file => $path)   # atomic file-backed snapshot
sub save {
    my ($state, %opt) = @_;
    my $blob = _marshal($state);

    if (defined $opt{file}) {
        my $path = $opt{file};
        my $tmp  = "$path.tmp.$$." . int(rand(1_000_000));
        my $csum = _fnv1a_32($blob);
        my $hdr  = FILE_MAGIC . pack('NN', length($blob), $csum);
        open my $fh, '>:raw', $tmp
            or die "MT::Checkpoint::save: cannot open $tmp: $!\n";
        print $fh $hdr, $blob
            or do { close $fh; unlink $tmp; die "MT::Checkpoint::save: write failed: $!\n" };
        close $fh
            or do { unlink $tmp; die "MT::Checkpoint::save: close failed: $!\n" };
        rename($tmp, $path)
            or do { unlink $tmp; die "MT::Checkpoint::save: rename to $path failed: $!\n" };
        return 1;
    }

    # Mint a capability holding the serialized checkpoint snapshot
    my $cap = MT::Cap::mint($blob);

    if ($CTX && $CTX->{store} && defined $CTX->{pair_id} && !defined $opt{name}) {
        my $pid = $CTX->{pair_id};
        $CTX->{store}->set("p:$pid:blob", $blob);
        my $next_seq = ($CTX->{seq} // 0) + 1;
        $CTX->{seq} = $next_seq;
        $CTX->{store}->set("p:$pid:seq", $next_seq);
        $CTX->{last_cap} = $cap;
        return $next_seq;
    }

    my $key = defined $opt{name} ? $opt{name} : '_default';
    $LOCAL_MEM{$key} = $cap;
    return 1;
}

# Restore the most recent checkpoint:
#   my $state = MT::Checkpoint::restore();
#   my $state = MT::Checkpoint::restore(name => 'k');
#   my $state = MT::Checkpoint::restore(file => $path);
sub restore {
    my (%opt) = @_;

    if (defined $opt{file}) {
        my $path = $opt{file};
        return undef unless -e $path;
        open my $fh, '<:raw', $path
            or die "MT::Checkpoint::restore: cannot open $path: $!\n";
        local $/;
        my $raw = <$fh>;
        close $fh;
        return undef unless defined $raw && length($raw) >= 13;
        my $magic = substr($raw, 0, 5);
        die "MT::Checkpoint::restore: invalid checkpoint header in $path\n"
            unless $magic eq FILE_MAGIC;
        my ($len, $csum) = unpack('NN', substr($raw, 5, 8));
        my $blob = substr($raw, 13);
        die "MT::Checkpoint::restore: truncated checkpoint in $path\n"
            unless length($blob) == $len;
        die "MT::Checkpoint::restore: checksum mismatch in $path\n"
            unless _fnv1a_32($blob) == $csum;
        return _unmarshal($blob);
    }

    if ($CTX && $CTX->{store} && defined $CTX->{pair_id} && !defined $opt{name}) {
        my $pid = $CTX->{pair_id};
        my $blob = $CTX->{store}->get("p:$pid:blob");
        return undef unless defined $blob && length $blob;
        my $cap = MT::Cap::mint($blob);
        $CTX->{last_cap} = $cap;
        return _unmarshal(MT::Cap::get($cap));
    }

    my $key = defined $opt{name} ? $opt{name} : '_default';
    my $cap = $LOCAL_MEM{$key};
    return undef unless defined $cap;
    my $blob = MT::Cap::get($cap);
    return _unmarshal($blob);
}

# Clear a checkpoint (in-memory, active pair, or file-backed).
sub clear {
    my (%opt) = @_;
    if (defined $opt{file}) {
        unlink $opt{file} if -e $opt{file};
        return 1;
    }
    if ($CTX && $CTX->{store} && defined $CTX->{pair_id} && !defined $opt{name}) {
        my $pid = $CTX->{pair_id};
        $CTX->{store}->delete("p:$pid:blob");
        $CTX->{store}->delete("p:$pid:seq");
        $CTX->{seq} = 0;
        $CTX->{last_cap} = undef;
        return 1;
    }
    my $key = defined $opt{name} ? $opt{name} : '_default';
    delete $LOCAL_MEM{$key};
    return 1;
}

1;

__END__

=head1 NAME

MT::Checkpoint - explicit data-only checkpoints for process pairs and persistence

=head1 SYNOPSIS

    use MT::Checkpoint;

    # Inside an MT::Pool(pairs => 1) job:
    my $state = MT::Checkpoint::restore() // { step => 0, acc => 0 };
    while ($state->{step} < 10_000) {
        $state->{acc} += ++$state->{step};
        MT::Checkpoint::save($state) if $state->{step} % 1000 == 0;
    }

    # File-backed checkpoint:
    MT::Checkpoint::save($state, file => '/tmp/job.ckpt');
    my $restored = MT::Checkpoint::restore(file => '/tmp/job.ckpt');

=head1 DESCRIPTION

Implements NonStop-style data-only checkpoints.  In a paired pool
(C<< MT::Pool->new(pairs => 1) >>), C<save($state)> marshals C<$state> into an
C<MT::Cap> capability stored for the designated backup worker.  When the
primary worker faults, the supervisor hands the job to the backup worker, which
calls C<restore()> to resume from the last committed checkpoint.  C stacks and
fibers are not transparently checkpointed.

=cut

package MT::SharedHash;

use strict;
use warnings;

# A hash of values that every worker can read and write.  The storage is in the
# core (mt_chan.c): 256 shards, each under its own lock.

sub new {
    my ($class) = @_;
    return bless { id => MT::SharedHash::_new() }, $class;
}

sub attach {
    my ($class, $id) = @_;
    return bless { id => $id }, $class;
}

sub id { $_[0]{id} }

sub get    { MT::SharedHash::_get($_[0]{id}, $_[1]) }
sub set    { MT::SharedHash::_set($_[0]{id}, $_[1], $_[2]); return $_[2] }
sub delete { MT::SharedHash::_delete($_[0]{id}, $_[1]) ? 1 : 0 }
sub exists { MT::SharedHash::_exists($_[0]{id}, $_[1]) ? 1 : 0 }

# A snapshot of the keys, shard by shard: a key set during the call may or may
# not be in it.
sub keys { MT::SharedHash::_keys($_[0]{id}) }

1;

__END__

=head1 NAME

MT::SharedHash - a hash shared between workers, sharded by key

=head1 SYNOPSIS

    my $h = MT::SharedHash->new;
    $h->set(alpha => 1);
    my $v = $h->get('alpha');
    $h->delete('alpha') if $h->exists('alpha');
    my @keys = $h->keys;

=head1 DESCRIPTION

Each key lives in one of 256 shards, and a shard has its own lock, so workers
writing different keys rarely wait for each other.  Values are copied in and
out (see L<MT>).

=head1 LIMITS

C<keys> is a snapshot taken shard by shard, so it is not a consistent view of
the whole hash while other workers write.  The buckets per shard are fixed (64),
so a very large hash has long chains; the storage is not resized.  Nested
references croak C<MT_UNSHAREABLE_REF>.  Hashes are never freed.

=cut

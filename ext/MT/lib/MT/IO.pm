package MT::IO;

use strict;
use warnings;

# The :async helpers are XSUBs of the core (perlio_async.c): MT::IO::stat,
# splice, fsync and aio_stats.  This file documents them.

1;

__END__

=head1 NAME

MT::IO - the :async layer's descriptor operations

=head1 SYNOPSIS

    open my $in, '<:async', $file or die;
    my @st = MT::IO::stat($file);           # stat's thirteen values
    MT::IO::splice($in, $out, 65536);       # bytes moved in the kernel
    MT::IO::fsync($out);                    # true when flushed to disk
    my %stats = MT::IO::aio_stats();        # the ring's counters

=head1 DESCRIPTION

C<stat> is STATX on the ring; C<splice> moves bytes between two descriptors
(both handles are flushed first, and a read-ahead in the input is not seen);
C<fsync> is FSYNC.  C<aio_stats> returns submitted, completed, parked, drained,
buffers, and pending as pairs.  The layer itself is described in perlio.

=cut

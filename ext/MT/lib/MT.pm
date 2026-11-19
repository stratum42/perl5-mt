package MT;

use strict;
use warnings;

our $VERSION = '0.01';

require XSLoader;
XSLoader::load('MT', $VERSION);

die "MT: this perl was built without the perl5-mt core (mt_chan.c,\n    mt_fiber.c, perlio_async.c)\n"
    unless _core_present();

use MT::Channel;
use MT::Cell;
use MT::SharedHash;
use MT::Pool;
use MT::Fiber;
use MT::IO;

1;

__END__

=head1 NAME

MT - shared values, channels, a worker pool and fibers for perl5-mt

=head1 SYNOPSIS

    use MT;
    MT::Image::freeze();                    # workers need the image
    my $pool = MT::Pool->new(workers => 4);
    my @squares = $pool->map(\&square, 1 .. 100);
    $pool->shutdown;

    sub square { $_[0] * $_[0] }

=head1 DESCRIPTION

The classes are L<MT::Channel>, L<MT::Cell>, L<MT::SharedHash>, L<MT::Pool>,
L<MT::Fiber> and L<MT::IO>.  Shared objects are named by an integer id, which
can be passed to a worker as an argument: the worker attaches to it with
C<attach>.  Values that cross a channel, a cell or a shared hash are copied:
scalars, capabilities and shared memory pass; a reference to a mutable value
croaks C<MT_UNSHAREABLE_REF>.

=cut

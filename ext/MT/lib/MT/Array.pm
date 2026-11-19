package MT::Array;
use strict;
use warnings;
use MT ();

our $VERSION = '0.01';

use Exporter 'import';
our @EXPORT_OK = qw(
    packed pack unpack is_packed unpack_count
    sum min max dot map_add slice count find
    vec_add vec_mul axpy dot_i8
);

1;

__END__

=head1 NAME

MT::Array - Opt-in packed typed arrays and SIMD reductions

=head1 SYNOPSIS

    use MT::Array;

    my $a = MT::Array::packed('IV', 1, 2, 3, 4);
    my $s = MT::Array::sum($a);
    my $m = MT::Array::min($a);
    my $x = MT::Array::max($a);

    my @plain = (1.5, 2.5, 3.5);
    MT::Array::pack(\@plain, 'NV');
    my $d = MT::Array::dot(\@plain, \@plain);
    MT::Array::unpack(\@plain);

=head1 DESCRIPTION

C<MT::Array> provides opt-in packed typed arrays (C<IV>, C<NV>, C<U8>) stored
in contiguous native buffers behind Perl's C<AV> interface, together with
correctly-rounded and bit-identical SIMD/accelerated reductions and vector
operations (C<sum>, C<min>, C<max>, C<dot>, C<map_add>, C<slice>, C<count>,
C<find>, C<vec_add>, C<vec_mul>, C<axpy>, C<dot_i8>).

Floating-point C<sum>, C<dot>, C<vec_add>, C<vec_mul>, C<axpy>, and C<dot_i8>
dispatch transparently through the native acceleration runtime (see
L<MT::Accel>), using macOS Accelerate Ozaki-split correctly-rounded reductions
when enabled and above threshold, and falling back to the exact scalar twin
under C<PERL_ACCEL=off> or C<PERL_FORCE_SCALAR=1>.

Any raw C<AvARRAY> access (such as C<splice>, C<sort>, list assignment, or
C<Storable>) permanently unpacks a packed array into standard C<SV*> slots.

=cut

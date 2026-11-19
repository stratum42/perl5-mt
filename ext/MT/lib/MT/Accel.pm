package MT::Accel;
use strict;
use warnings;
use MT ();

our $VERSION = '0.01';

use Exporter 'import';
our @EXPORT_OK = qw(
    backends active enabled simd_level simd_caps stats reset_stats
);

1;

__END__

=head1 NAME

MT::Accel - Native acceleration backend and CPU SIMD ISA introspection

=head1 SYNOPSIS

    use MT::Accel;

    my $active   = MT::Accel::active();
    my $backends = MT::Accel::backends();
    my $level    = MT::Accel::simd_level();
    my $caps     = MT::Accel::simd_caps();
    my $stats    = MT::Accel::stats();

=head1 DESCRIPTION

C<MT::Accel> exposes the runtime state of the perl5-mt native acceleration
and transparent offload framework as well as the extended CPU SIMD ISA
detection layer.

=head2 Functions

=over 4

=item C<backends()>

Returns an arrayref of hashrefs describing each registered backend:
C<name>, C<description>, C<device_class> (C<cpu>, C<gpu>, C<npu>, C<coproc>),
C<priority>, C<available>, C<active>, C<caps>, C<min_len_f64>, C<min_len_i8>,
and C<min_len_nx>.

=item C<active()>

Returns the name of the currently active acceleration backend (for example,
C<accelerate> on macOS in C<PERL_ACCEL=auto> mode, or C<scalar> when
C<PERL_ACCEL=off> or C<PERL_FORCE_SCALAR=1>).

=item C<enabled()>

Returns true if transparent offload is enabled, false under C<PERL_ACCEL=off>
or C<PERL_FORCE_SCALAR=1>.

=item C<simd_level()>

Returns the name of the highest detected extended CPU SIMD ISA tier
(e.g. C<neon>, C<avx2>, C<avx512>, C<vsx3>, C<scalar>).

=item C<simd_caps()>

Returns a hashref of fine-grained CPU ISA capability flags (including the
raw C<bits> bitmask and boolean keys such as C<neon>, C<neon_dotprod>,
C<apple_amx>, C<sse2>, C<avx2>, C<avx512f>, C<vsx>, C<s390x_vx>, etc.).

=item C<stats()>

Returns a hashref with dispatch counters: C<offload_calls>,
C<fallback_calls>, and C<last_backend>.

=item C<reset_stats()>

Resets the dispatch counters to zero.

=back

=cut

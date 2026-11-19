package MT::XSTest;

use strict;
use warnings;

our $VERSION = '0.01';

require XSLoader;
XSLoader::load('MT::XSTest', $VERSION);

1;
__END__

=head1 NAME

MT::XSTest - a thread-safe XS module for the perl5-mt dispatcher tests

=head1 SYNOPSIS

    use MT::XSTest;
    MT::XSTest::add(2, 3);          # 5, in-process in every worker
    MT::XS::is_threadsafe(\&MT::XSTest::add);   # true

=head1 DESCRIPTION

This module is built with C<-DPERL_XS_THREADSAFE=1> and
C<PERL_NO_GET_CONTEXT>, so its shared object carries the
C<mt_threadsafe_boot_MT__XSTest> marker and C<DynaLoader> loads it
in-process in worker interpreters; every XSUB it defines is stamped
C<CVf_MT_THREADSAFE>.  It exists for the tests in F<t/mt> and
F<ext/MT-XSTest/t>; it is not installed.

=head2 Functions

C<add($a, $b)>, C<bump()> (a per-interpreter C<MY_CXT> counter, reset by
C<CLONE>), C<echo($sv)>, C<worker_id()>.

=head1 SEE ALSO

F<MT_README.md>, L<perlguts/"perl5-mt: thread-safe XS modules">.

=cut

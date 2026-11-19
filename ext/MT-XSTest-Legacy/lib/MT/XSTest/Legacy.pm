package MT::XSTest::Legacy;

use strict;
use warnings;

our $VERSION = '0.01';
our $setting = 'default';

require XSLoader;
XSLoader::load('MT::XSTest::Legacy', $VERSION);

# the Perl half of the module: exists wherever the module is loaded, so XS
# code in a compartment can call it by name
sub perl_side { my ($x) = @_; return "perl_side($x)" }

# class methods for the MT::XS::remote proxy tests
sub make { my ($class, %fields) = @_; return new_object('MT::XSTest::Legacy::Obj', %fields) }
sub add  { my ($class, @n) = @_; return sum(@n) }

package MT::XSTest::Legacy::Obj;
sub get   { my ($self, $k) = @_; return $self->{$k} }
sub set   { my ($self, $k, $v) = @_; $self->{$k} = $v; return $self }
sub class { return ref $_[0] }

1;
__END__

=head1 NAME

MT::XSTest::Legacy - a legacy XS module for the perl5-mt dispatcher tests

=head1 SYNOPSIS

    # inside a worker: loaded into an XS compartment
    require MT::XSTest::Legacy;
    MT::XSTest::Legacy::sum(1, 2, 3);          # a remote call
    MT::XS::is_remote(\&MT::XSTest::Legacy::sum);   # true

=head1 DESCRIPTION

This module is built without C<PERL_XS_THREADSAFE>, so its shared object
has no marker: while workers are active C<DynaLoader> loads it into an XS
compartment and every function becomes a host stub (C<CVf_MT_REMOTE>).
It uses C<MY_CXT> without a C<CLONE> method on purpose, which makes
C<MT::Image::freeze> refuse an image that contains it
(C<MT_MY_CXT_WITHOUT_CLONE>) unless it is listed in
C<%MT::XS::inprocess_ok>.  With C<MT_XSTEST_HOOK> set in the environment
its boot installs a keyword plugin, which a compartment refuses
(C<MT_COMPARTMENT_HOOK_UNSUPPORTED>).

It exists for the tests in F<t/mt>; it is not installed.

=head2 Functions

C<echo(@args)>, C<sum(@n)>, C<big_string($n)>, C<length_of($s)>,
C<bump()> (C<MY_CXT>), C<global_bump()> (a process-global counter),
C<pid()>, C<callback($cb, @args)>, C<callback_scalar($cb, $arg)>,
C<callback_catch($cb)>, C<croak_with($msg)>, C<die_object($class)>,
C<modify_arg($sv, $new)>, C<modify_ref(\$sv, $new)>,
C<new_object($class, %fields)>, C<new_array_object($class, @list)>,
C<obj_get($obj, $key)>, C<obj_class($obj)>, C<objects_alive()>,
C<same_object($a, $b)>, C<magic_on_arg($sv)>, C<pkg_var()>,
C<code_ref()>, C<call_perl_side($x)>, C<sleep_ms($ms)>,
C<exit_now($code)>; C<MT::XSTest::Legacy::Obj::DESTROY> counts objects.

=head1 SEE ALSO

F<MT_README.md>, L<MT::XSTest>.

=cut

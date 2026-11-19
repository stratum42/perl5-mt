#!./perl
# perl5-mt: the legacy fixture loads in-process when no worker is active.

use strict;
use warnings;
use Test::More tests => 6;

use_ok('MT::XSTest::Legacy');

is(MT::XSTest::Legacy::sum(1, 2, 3), 6, 'sum');
is_deeply([MT::XSTest::Legacy::echo(1, "two", [3])], [1, "two", [3]], 'echo');
ok(!MT::XS::is_threadsafe(\&MT::XSTest::Legacy::sum),
   'no PERL_XS_THREADSAFE marker: not stamped threadsafe');
ok(!MT::XS::is_remote(\&MT::XSTest::Legacy::sum),
   '... and in-process while no worker is active');
is(MT::XSTest::Legacy::call_perl_side("x"), "perl_side(x)", 'the Perl half is called by name');

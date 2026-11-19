#!./perl
# perl5-mt: a PERL_XS_THREADSAFE module loads in-process and its XSUBs are
# stamped CVf_MT_THREADSAFE.

use strict;
use warnings;
use Test::More tests => 7;

use_ok('MT::XSTest');

is(MT::XSTest::add(2, 3), 5, 'add');
is(MT::XSTest::echo("x"), "x", 'echo');
is(MT::XSTest::bump(), 1, 'MY_CXT counter');
is(MT::XSTest::bump(), 2, '... counts');
ok(MT::XS::is_threadsafe(\&MT::XSTest::add),
   'XSUBs of a PERL_XS_THREADSAFE module carry CVf_MT_THREADSAFE');
ok(!MT::XS::is_remote(\&MT::XSTest::add), '... and are not remote stubs');

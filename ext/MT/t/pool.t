#!./perl
# perl5-mt: MT::Pool (ext/MT).  The pool's own behaviour in the
# child perls of t/mt/pool.t; here the module's Perl side: the message format,
# the named-sub rule, and the argument rule.

use FindBin;
use lib "$FindBin::Bin/../lib";
use Test::More;

use strict;
use warnings;
use MT::Pool;

plan tests => 5;

my @fields = MT::Pool::_unpack(MT::Pool::_pack('a:b', '', 'x' x 300));
is(join('|', map { length } @fields), '3|0|300', 'messages round-trip, length-prefixed');
my $anon = sub { 1 };
eval { MT::Pool::_sub_name($anon) };
like($@, qr/named sub/, 'an anonymous sub is refused by name');
eval { MT::Pool::_sub_name(\&MT::Pool::_pack) };
is($@, '', 'a named sub gets its name');
eval { MT::Pool::_check_args([1]) };
like($@, qr/plain values/, 'a reference argument is refused');
eval { MT::Pool->new(workers => 0) };
like($@, qr/at least 1/, 'a pool needs at least one worker');

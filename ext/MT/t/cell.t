#!./perl
# perl5-mt: MT::Cell (mt_chan.c).  get and set, cas on equal values,
# update, and the refusal of a reference.

use FindBin;
use lib "$FindBin::Bin/../lib";
use Test::More;
use MT::Cell;

use strict;
use warnings;

plan tests => 7;

my $c = MT::Cell->new(5);
is($c->get, 5, 'a new cell holds its value');
is($c->cas(5, 6), 1, 'cas succeeds when the value is the expected one');
is($c->cas(5, 7), 0, 'cas fails when it is not');
is($c->get, 6, 'and the value stayed');
is($c->update(sub { $_[0] * 10 }), 60, 'update returns the new value');
$c->set('text');
is($c->get, 'text', 'set replaces the value');
eval { $c->set([]) };
like($@, qr/MT_UNSHAREABLE_REF/, 'a reference croaks MT_UNSHAREABLE_REF');

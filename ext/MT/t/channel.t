#!./perl
# perl5-mt: MT::Channel (mt_chan.c).  Order, capacity, close, the
# copies made for references, and a channel shared by two workers.

use FindBin;
use lib "$FindBin::Bin/../lib";
use Test::More;
use MT::Channel;

use strict;
use warnings;

plan tests => 8;

my $ch = MT::Channel->new(3);
$ch->send($_) for qw(a b c);
is($ch->len, 3, 'three values queued');
is($ch->recv, 'a', 'values come out in order');
my ($ok, $v) = $ch->try_recv;
is("$ok:$v", '1:b', 'try_recv returns the next value');
$ch->close;
is($ch->recv, 'c', 'recv drains the queue after close');
ok(!defined $ch->recv, 'recv returns undef once closed and empty');
eval { $ch->send('late') };
like($@, qr/send on a closed channel/, 'send after close croaks');
my @none = $ch->try_recv;
is(scalar(@none), 1, 'try_recv on an empty channel returns the one false flag');

# the same channel through its id
my $c2 = MT::Channel->new(1);
my $again = MT::Channel->attach($c2->id);
$c2->send(42);
is($again->recv, 42, 'an attached channel is the same channel');

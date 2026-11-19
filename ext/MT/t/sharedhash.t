#!./perl
# perl5-mt: MT::SharedHash (mt_chan.c).  Keys across the shards, a
# snapshot of the keys, delete, and the exists test.

use FindBin;
use lib "$FindBin::Bin/../lib";
use Test::More;
use MT::SharedHash;

use strict;
use warnings;

plan tests => 7;

my $h = MT::SharedHash->new;
$h->set("key$_" => $_ * 2) for 1 .. 1000;
is($h->get('key7'), 14, 'get returns the stored value');
ok($h->exists('key999'), 'exists is true for a stored key');
ok(!$h->exists('nope'), 'exists is false for a missing key');
my @keys = $h->keys;
is(scalar(@keys), 1000, 'keys lists every key');
ok($h->delete('key7'), 'delete returns true for a stored key');
ok(!defined $h->get('key7'), 'the deleted key is gone');
my %seen = map { $_ => 1 } $h->keys;
is(scalar(keys %seen), 999, 'the snapshot has the keys left');

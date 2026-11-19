use strict;
use warnings;
use Test::More;
use MT::Decimal;

my $a = MT::Decimal->new('0.1');
my $b = MT::Decimal->new('0.2');
my $c = $a + $b;
is("$c", '0.3', '0.1 + 0.2 == 0.3 exactly');
ok($c == MT::Decimal->new('0.3'), 'numeric equality');
ok($c != MT::Decimal->new('0.4'), 'numeric inequality');

my $big = MT::Decimal->new('1234567890123456789012345678901234');
is("$big", '1234567890123456789012345678901234', '34-digit integer exact');

my $q = MT::Decimal->new('1') / MT::Decimal->new('3');
is("$q", '0.3333333333333333333333333333333333', '1/3 to 34 digits');

my $m = MT::Decimal->new('19.99') * MT::Decimal->new('3') + MT::Decimal->new('0.03');
is("$m", '60.00', 'money-style arithmetic preserves scale');

is(MT::Decimal->new('2.5')->quantize(0, MT::Decimal::ROUND_HALF_EVEN)->to_string, '2', 'half-even 2.5 -> 2');
is(MT::Decimal->new('3.5')->quantize(0, MT::Decimal::ROUND_HALF_EVEN)->to_string, '4', 'half-even 3.5 -> 4');
is(MT::Decimal->new('2.5')->quantize(0, MT::Decimal::ROUND_HALF_UP)->to_string, '3', 'half-up 2.5 -> 3');
is(MT::Decimal->new('2.9')->quantize(0, MT::Decimal::ROUND_DOWN)->to_string, '2', 'round-down 2.9 -> 2');
is(MT::Decimal->new('-2.1')->quantize(0, MT::Decimal::ROUND_FLOOR)->to_string, '-3', 'floor -2.1 -> -3');
is(MT::Decimal->new('2.1')->quantize(0, MT::Decimal::ROUND_CEIL)->to_string, '3', 'ceil 2.1 -> 3');

my $nan = MT::Decimal->new('NaN');
ok($nan->is_nan, 'NaN detected');
ok(!($nan == $nan), 'NaN != NaN');
is(length($c->to_bytes), 16, 'BID128 encoding is 16 bytes');
ok(MT::Decimal::backend() =~ /^(?:dfp|bid128)$/, 'backend reported');

done_testing;

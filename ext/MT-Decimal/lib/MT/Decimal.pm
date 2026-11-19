package MT::Decimal;

use strict;
use warnings;

our $VERSION = '0.01';

use constant {
    ROUND_HALF_EVEN => 0,
    ROUND_HALF_UP   => 1,
    ROUND_DOWN      => 2,
    ROUND_FLOOR     => 3,
    ROUND_CEIL      => 4,
};

our $ROUND_MODE = ROUND_HALF_EVEN;

require XSLoader;
XSLoader::load('MT::Decimal', $VERSION);

use overload
    '+'   => sub { my ($a, $b, $swap) = @_; $swap ? MT::Decimal::add($b, $a, $ROUND_MODE) : MT::Decimal::add($a, $b, $ROUND_MODE) },
    '-'   => sub { my ($a, $b, $swap) = @_; $swap ? MT::Decimal::sub($b, $a, $ROUND_MODE) : MT::Decimal::sub($a, $b, $ROUND_MODE) },
    '*'   => sub { my ($a, $b, $swap) = @_; $swap ? MT::Decimal::mul($b, $a, $ROUND_MODE) : MT::Decimal::mul($a, $b, $ROUND_MODE) },
    '/'   => sub { my ($a, $b, $swap) = @_; $swap ? MT::Decimal::div($b, $a, $ROUND_MODE) : MT::Decimal::div($a, $b, $ROUND_MODE) },
    '%'   => sub { my ($a, $b, $swap) = @_; $swap ? MT::Decimal::mod($b, $a, $ROUND_MODE) : MT::Decimal::mod($a, $b, $ROUND_MODE) },
    'neg' => sub { MT::Decimal::neg($_[0]) },
    'abs' => sub { MT::Decimal::abs($_[0]) },
    '<=>' => sub { my ($a, $b, $swap) = @_; $swap ? MT::Decimal::cmp($b, $a) : MT::Decimal::cmp($a, $b) },
    '=='  => sub { my $c = MT::Decimal::cmp($_[0], $_[1]); defined($c) && $c == 0 },
    '!='  => sub { my $c = MT::Decimal::cmp($_[0], $_[1]); !defined($c) || $c != 0 },
    '<'   => sub { my ($a, $b, $swap) = @_; my $c = $swap ? MT::Decimal::cmp($b, $a) : MT::Decimal::cmp($a, $b); defined($c) && $c < 0 },
    '<='  => sub { my ($a, $b, $swap) = @_; my $c = $swap ? MT::Decimal::cmp($b, $a) : MT::Decimal::cmp($a, $b); defined($c) && $c <= 0 },
    '>'   => sub { my ($a, $b, $swap) = @_; my $c = $swap ? MT::Decimal::cmp($b, $a) : MT::Decimal::cmp($a, $b); defined($c) && $c > 0 },
    '>='  => sub { my ($a, $b, $swap) = @_; my $c = $swap ? MT::Decimal::cmp($b, $a) : MT::Decimal::cmp($a, $b); defined($c) && $c >= 0 },
    '""'  => sub { MT::Decimal::to_string($_[0]) },
    '0+'  => sub { MT::Decimal::to_nv($_[0]) },
    'bool'=> sub { !MT::Decimal::is_zero($_[0]) },
    fallback => 1;

sub round_mode {
    my ($self, $mode) = @_;
    $ROUND_MODE = $mode if defined $mode;
    return $ROUND_MODE;
}

1;
__END__

=head1 NAME

MT::Decimal - IEEE 754-2008 decimal128 arithmetic for perl5-mt

=head1 SYNOPSIS

    use MT::Decimal;

    my $a = MT::Decimal->new('0.1');
    my $b = MT::Decimal->new('0.2');
    my $c = $a + $b;
    print "$c\n";                    # 0.3
    print $c == MT::Decimal->new('0.3'); # 1

    my $cents = $c->quantize(-2, MT::Decimal::ROUND_HALF_UP);

=head1 DESCRIPTION

C<MT::Decimal> implements 34-digit IEEE 754-2008 decimal128 floating-point
arithmetic with operator overloading, explicit rounding modes, quantization,
and canonical 16-byte BID128 serialization for cross-host reproducibility.

=cut

package MT::Cell;

use strict;
use warnings;

# One value that every worker can read and replace.  The primitives are in the
# core (mt_chan.c): get, set and cas operate lock-free on the cell's atomic
# body pointer with hazard-pointer reclamation.

sub new {
    my ($class, $value) = @_;
    return bless { id => MT::Cell::_new($value) }, $class;
}

sub attach {
    my ($class, $id) = @_;
    return bless { id => $id }, $class;
}

sub id { $_[0]{id} }

sub get { MT::Cell::_get($_[0]{id}) }

sub set {
    my ($self, $value) = @_;
    MT::Cell::_set($self->{id}, $value);
    return $value;
}

# Replaces the value with $new when it is $expected (the same string or
# number); returns 1 when it did.
sub cas {
    my ($self, $expected, $new) = @_;
    return MT::Cell::_cas($self->{id}, $expected, $new) ? 1 : 0;
}

# Replaces the value with $f->($old), retrying while another change came
# first.  Returns the new value.
sub update {
    my ($self, $f) = @_;
    my $old = $self->get;
    while (1) {
        my $new = $f->($old);
        return $new if $self->cas($old, $new);
        $old = $self->get;
    }
}

1;

__END__

=head1 NAME

MT::Cell - one shared value, replaced atomically

=head1 SYNOPSIS

    my $cell = MT::Cell->new(0);
    $cell->update(sub { $_[0] + 1 });       # atomic increment
    my $old = $cell->get;
    $cell->cas($old, $old + 10) or warn "lost a race";

=head1 DESCRIPTION

C<cas> compares the stored value with C<$expected> as strings or numbers
(the same kind and contents) and atomically swaps the cell's body pointer to
C<$new> via lock-free compare-and-swap when they are equal.  C<update> loops
on C<cas>.

=head1 LIMITS

A reference to a mutable value croaks C<MT_UNSHAREABLE_REF>; there is no
automatic promotion of nested references.  Cells are never freed.

=cut

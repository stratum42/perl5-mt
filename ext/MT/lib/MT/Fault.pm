package MT::Fault;

use strict;
use warnings;

BEGIN {
    require MT unless defined &MT::Fault::_last;
}

our @OBSERVERS;
our $LAST_SEEN_SEQ = 0;

sub last {
    return MT::Fault::_last();
}

sub count {
    return MT::Fault::_count();
}

sub sites {
    return MT::Fault::_sites();
}

sub reset {
    $LAST_SEEN_SEQ = 0;
    @OBSERVERS = ();
    MT::Fault::_reset();
    return 1;
}

# Register a process-wide observer callback: MT::Fault::on(\&cb).
# Passing undef clears all registered observers.
sub on {
    my ($cb) = @_;
    if (!defined $cb) {
        @OBSERVERS = ();
        return 0;
    }
    die "MT::Fault::on: callback must be a CODE reference\n"
        unless ref $cb eq 'CODE';
    push @OBSERVERS, $cb;
    return scalar @OBSERVERS;
}

# Drain any fault events recorded since the last poll and dispatch them
# to all registered observers.
sub poll {
    my $pair = MT::Fault::_drain($LAST_SEEN_SEQ);
    my ($new_seq, $events) = @$pair;
    $LAST_SEEN_SEQ = $new_seq;
    my $n = 0;
    for my $ev (@$events) {
        $n++;
        for my $cb (@OBSERVERS) {
            $cb->($ev);
        }
    }
    return $n;
}

1;

__END__

=head1 NAME

MT::Fault - fail-fast policy and process-wide fault event model

=head1 SYNOPSIS

    use MT::Fault;
    MT::Fault::on(sub {
        my ($f) = @_;
        warn "fault #$f->{seq} [$f->{cause}] $f->{unit_kind}:$f->{unit_id}: $f->{message}\n";
    });
    MT::Fault::poll();
    my $last = MT::Fault::last();

=head1 DESCRIPTION

Exposes the process-wide fault event log maintained by C<mt_fault.c>.
Recoverable faults stop the offending worker or compartment immediately and
record an event observable via C<last()> and C<on(\&cb)>; fatal faults
terminate the process immediately.

=cut

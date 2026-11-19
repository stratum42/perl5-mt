package MT::Supervisor;

use strict;
use warnings;

BEGIN {
    require MT unless defined &MT::Supervisor::_tick;
}
use MT::Fault ();

# Scan one pending-bit word of the liveness vector table, mark timed-out
# units as faulted, and dispatch any newly recorded fault events to
# MT::Fault observers.
sub tick {
    my $faulted = MT::Supervisor::_tick();
    MT::Fault::poll();
    return $faulted;
}

# Return an arrayref of hashrefs describing each tracked unit:
# { slot, unit, unit_kind, unit_id, generation, last_beat_ns, age_ms,
#   missed_beats, beat_count, state }.
sub status {
    return MT::Supervisor::_status();
}

# Post a heartbeat for the current worker unit.
sub beat {
    MT::Supervisor::_beat();
    return 1;
}

# Return the active heartbeat configuration: { heartbeat_ms, heartbeat_miss }.
sub config {
    return MT::Supervisor::_config();
}

1;

__END__

=head1 NAME

MT::Supervisor - worker and compartment liveness supervision over krt_vec

=head1 SYNOPSIS

    use MT::Supervisor;
    my $faulted = MT::Supervisor::tick();
    my $units   = MT::Supervisor::status();

=head1 DESCRIPTION

Every worker and compartment posts heartbeats to the process liveness vector
table (C<krt/krt_vec.h>) on the C<PERL_MT_HEARTBEAT_MS> schedule (default
100 ms).  C<tick()> scans one pending-bit word per call and marks units
faulted after C<PERL_MT_HEARTBEAT_MISS> (default 3) missed beats, recording
a fault event in L<MT::Fault>.

=cut

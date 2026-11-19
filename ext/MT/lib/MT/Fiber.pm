package MT::Fiber;

use strict;
use warnings;

# The fiber primitives are XSUBs of the core (mt_fiber.c): MT::Fiber::spawn,
# resume, yield, join, schedule, ready, wake, park, pass, run, preempt,
# is_finished, current, count, stats and sched_stats.  This file documents them.

1;

__END__

=head1 NAME

MT::Fiber - stackful fibers, and the scheduler that runs them

=head1 SYNOPSIS

    my @ids = map { MT::Fiber::schedule(sub { ... }) } 1 .. 64;
    MT::Fiber::run();                       # runs them to their ends
    my @results = map { MT::Fiber::join($_) } @ids;

=head1 DESCRIPTION

C<schedule> makes a scheduled fiber; C<run> is the loop that runs the ready
fibers until none is ready.  A scheduled fiber that waits (an aio read, a
remote XS call, a channel) parks, and the loop runs the others.  C<spawn> and
C<resume> are the unscheduled form: the fiber runs when resumed.

=cut

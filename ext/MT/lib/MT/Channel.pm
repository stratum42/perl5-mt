package MT::Channel;

use strict;
use warnings;

BEGIN {
    require MT unless defined &MT::Channel::_named_lookup;
}

# A bounded queue of values, shared by every worker of the process.  The
# primitives are in the core (mt_chan.c): this class is the Perl face.

sub new {
    my ($class, @args) = @_;
    my ($capacity, $name);
    if (@args >= 2 && defined $args[0] && ($args[0] eq 'capacity' || $args[0] eq 'name')) {
        my %opt = @args;
        $capacity = $opt{capacity};
        $name     = $opt{name};
    }
    else {
        $capacity = $args[0];
    }
    $capacity = 1 unless defined $capacity;
    my $id = MT::Channel::_new($capacity);
    if (defined $name && length $name) {
        my ($rc, $existing) = MT::Channel::_named_register($name, $id, 1);
        die "MT::Channel: failed to register name '$name'\n" if $rc < 0;
    }
    return bless { id => $id, (defined $name ? (name => $name) : ()) }, $class;
}

# The channel an id names (as passed to a worker).
sub attach {
    my ($class, $id, $name) = @_;
    return bless { id => $id, (defined $name ? (name => $name) : ()) }, $class;
}

# Bind to a process-wide named channel, creating it atomically if absent.
sub named {
    my ($class, $name, $capacity) = @_;
    die "MT::Channel->named: name must be a non-empty string\n"
        unless defined $name && length $name;
    my $id = MT::Channel::_named_lookup($name);
    if (defined $id) {
        return $class->attach($id, $name);
    }
    $capacity = 64 unless defined $capacity;
    my $new_id = MT::Channel::_new($capacity);
    my ($rc, $actual_id) = MT::Channel::_named_register($name, $new_id, 0);
    die "MT::Channel->named: failed to register '$name'\n" if $rc < 0;
    return $class->attach($actual_id, $name);
}

sub unregister_name {
    my ($class, $name) = @_;
    return MT::Channel::_named_unregister($name);
}

sub named_list {
    my ($class) = @_;
    my $list = MT::Channel::_named_list();
    return wantarray ? @$list : $list;
}

sub id   { $_[0]{id} }
sub name { $_[0]{name} }

sub send {
    my ($self, $value) = @_;
    MT::Channel::_send($self->{id}, $value);
    return 1;
}

# The next value; waits while the channel is empty.  Returns undef once the
# channel is closed and drained.
sub recv { MT::Channel::_recv($_[0]{id}) }

# (1, $value) when a value was waiting, else (0).
sub try_recv {
    my ($self) = @_;
    my @r = MT::Channel::_try_recv($self->{id});
    return @r;
}

sub close {
    my ($self) = @_;
    MT::Channel::_close($self->{id});
    return 1;
}

sub len { MT::Channel::_len($_[0]{id}) }

1;

__END__

=head1 NAME

MT::Channel - a bounded queue shared between workers and fibers

=head1 SYNOPSIS

    my $ch = MT::Channel->new(64);          # capacity 64
    my $ev = MT::Channel->named('events');  # process-wide named endpoint
    $ch->send($value);                      # waits while full
    my $v = $ch->recv;                      # waits while empty
    my ($ok, $w) = $ch->try_recv;           # never waits
    $ch->close;                             # recv gives undef when empty

In a worker, C<< MT::Channel->attach($id) >> or C<< MT::Channel->named('events') >>
binds to the same channel created in the main program or another worker.

=head1 DESCRIPTION

A send blocks while the channel is full, a receive blocks while it is empty.
A scheduled fiber (see L<MT::Fiber>) parks instead of blocking its thread, and
the scheduler wakes it when the channel can go on.  A value is copied into
the channel (see L<MT>).

=head1 LIMITS

Channels are never freed: a channel lives until the process exits.

=cut

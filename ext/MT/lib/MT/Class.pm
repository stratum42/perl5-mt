package MT::Class;

use strict;
use warnings;

BEGIN {
    require MT unless defined &MT::Class::_create;
}

sub new {
    my ($class, %opt) = @_;
    my $name       = $opt{name} // 'class';
    my $cpu        = defined $opt{cpu} ? $opt{cpu} : 100;
    my $mem_bytes  = defined $opt{mem_bytes}
        ? $opt{mem_bytes}
        : (defined $opt{mem_mb} ? int($opt{mem_mb} * 1024 * 1024) : 0);
    my $io_ops     = defined $opt{io_ops} ? $opt{io_ops} : 0;
    my $io_win_ms  = defined $opt{io_window_ms} ? $opt{io_window_ms} : 1000;

    my $id = MT::Class::_create($name, $cpu, $mem_bytes, $io_ops, $io_win_ms);
    die "MT::Class::new: failed to allocate resource class slot\n" unless $id;

    return bless {
        id           => $id,
        name         => $name,
        cpu          => $cpu,
        mem_bytes    => $mem_bytes,
        io_ops       => $io_ops,
        io_window_ms => $io_win_ms,
    }, $class;
}

sub id   { $_[0]->{id} }
sub name { $_[0]->{name} }

sub bind_thread {
    my ($self) = @_;
    my $rc = MT::Class::_bind_thread($self->{id});
    return $rc == 0 ? 1 : 0;
}

sub charge_mem {
    my ($self, $bytes) = @_;
    my $rc = MT::Class::_charge_mem($self->{id}, $bytes);
    return $rc == 0 ? 1 : 0;
}

sub release_mem {
    my ($self, $bytes) = @_;
    my $rc = MT::Class::_release_mem($self->{id}, $bytes);
    return $rc == 0 ? 1 : 0;
}

sub charge_io {
    my ($self, $ops) = @_;
    $ops = 1 unless defined $ops;
    my $rc = MT::Class::_charge_io($self->{id}, $ops);
    return $rc == 0 ? 1 : 0;
}

sub charge_cpu {
    my ($self, $units) = @_;
    $units = 1 unless defined $units;
    my $rc = MT::Class::_charge_cpu($self->{id}, $units);
    return $rc == 0 ? 1 : 0;
}

sub stats {
    my ($self) = @_;
    return MT::Class::_stats($self->{id});
}

sub destroy {
    my ($self) = @_;
    return 0 unless defined $self->{id};
    my $rc = MT::Class::_destroy($self->{id});
    $self->{id} = undef;
    return $rc == 0 ? 1 : 0;
}

sub DESTROY {
    my ($self) = @_;
    return if MT::Worker::is_worker();
    $self->destroy if defined $self->{id};
}

1;

__END__

=head1 NAME

MT::Class - resource classes for worker groups (WLM tier)

=head1 SYNOPSIS

    use MT::Class;
    my $cls = MT::Class->new(
        name   => 'batch',
        cpu    => 30,
        mem_mb => 512,
        io_ops => 1000,
    );

=cut

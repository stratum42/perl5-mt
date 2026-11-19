package MT::ProcessPair;

use strict;
use warnings;

BEGIN {
    require MT unless defined &MT::ProcessPair::_open;
}

use constant {
    ROLE_NONE      => 0,
    ROLE_PRIMARY   => 1,
    ROLE_BACKUP    => 2,
    STATE_INIT     => 0,
    STATE_ACTIVE   => 1,
    STATE_FAILOVER => 2,
    STATE_STOPPED  => 3,
};

sub _role_num {
    my ($role) = @_;
    return ROLE_PRIMARY if !defined $role || $role eq 'primary' || $role eq '1';
    return ROLE_BACKUP  if $role eq 'backup' || $role eq '2';
    die "MT::ProcessPair: unknown role '$role'\n";
}

sub create {
    my ($class, %opt) = @_;
    my $path = $opt{path} // die "MT::ProcessPair::create: path required\n";
    my $name = $opt{name} // 'pair';
    my $img  = $opt{image_path} // '';
    my $hb   = defined $opt{hb_timeout_ms} ? $opt{hb_timeout_ms} : 300;
    my $sup  = $opt{supervisor} ? 1 : 0;

    my $id = MT::ProcessPair::_open($path, $name, $img, $hb, $sup, 1);
    die "MT::ProcessPair::create: failed to create shared pair state at $path\n"
        unless $id;

    return bless {
        id   => $id,
        path => $path,
        role => ROLE_NONE,
    }, $class;
}

sub attach {
    my ($class, %opt) = @_;
    my $path = $opt{path} // die "MT::ProcessPair::attach: path required\n";
    my $id = MT::ProcessPair::_open($path, '', '', 0, 0, 0);
    die "MT::ProcessPair::attach: failed to attach to $path\n" unless $id;

    return bless {
        id   => $id,
        path => $path,
        role => ROLE_NONE,
    }, $class;
}

sub id   { $_[0]->{id} }
sub path { $_[0]->{path} }

sub register_primary {
    my ($self) = @_;
    my $rc = MT::ProcessPair::_register($self->{id}, ROLE_PRIMARY);
    die "MT::ProcessPair::register_primary failed ($rc)\n" if $rc < 0;
    $self->{role} = ROLE_PRIMARY;
    return 1;
}

sub register_backup {
    my ($self) = @_;
    my $rc = MT::ProcessPair::_register($self->{id}, ROLE_BACKUP);
    die "MT::ProcessPair::register_backup failed ($rc)\n" if $rc < 0;
    $self->{role} = ROLE_BACKUP;
    return 1;
}

sub heartbeat {
    my ($self, $role) = @_;
    my $r = defined $role ? _role_num($role) : ($self->{role} || ROLE_PRIMARY);
    my $rc = MT::ProcessPair::_heartbeat($self->{id}, $r);
    die "MT::ProcessPair::heartbeat failed ($rc)\n" if $rc < 0;
    return 1;
}

sub checkpoint {
    my ($self, $blob) = @_;
    $blob = '' unless defined $blob;
    my $seq = MT::ProcessPair::_checkpoint($self->{id}, $blob);
    die "MT::ProcessPair::checkpoint failed\n" unless $seq;
    return $seq;
}

sub last_checkpoint {
    my ($self) = @_;
    return MT::ProcessPair::_last_checkpoint($self->{id});
}

sub check_failover {
    my ($self) = @_;
    my $rc = MT::ProcessPair::_check_failover($self->{id});
    die "MT::ProcessPair::check_failover failed ($rc)\n" if $rc < 0;
    return $rc;
}

sub status {
    my ($self) = @_;
    return MT::ProcessPair::_status($self->{id});
}

# Multi-key atomic commit (TMF analog) over shared pair state.
sub tx_commit {
    my ($self, $kv_href) = @_;
    my $rc = MT::ProcessPair::_tx_commit($self->{id}, $kv_href);
    return $rc == 0 ? 1 : 0;
}

sub tx_snapshot {
    my ($self) = @_;
    return MT::ProcessPair::_tx_snapshot($self->{id});
}

sub close {
    my ($self) = @_;
    return 0 unless defined $self->{id};
    my $rc = MT::ProcessPair::_close($self->{id});
    $self->{id} = undef;
    return $rc == 0 ? 1 : 0;
}

sub DESTROY {
    my ($self) = @_;
    return if MT::Worker::is_worker();
    $self->close if defined $self->{id};
}

1;

__END__

=head1 NAME

MT::ProcessPair - cross-process primary/backup pairs and supervisor pairs

=head1 DESCRIPTION

Provides cross-process primary/backup failover over a shared memory segment
(paired with an L<MT::SharedImage> path), a checksummed checkpoint ring,
atomic multi-key transactions, and redundant supervisor failover so a single
interpreter process is no longer a single point of failure.

=cut

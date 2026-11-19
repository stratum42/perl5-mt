package MT::Partition;

use strict;
use warnings;

use MT::Pool;
use MT::Namespace;
use MT::Class;

our %PARTITIONS;
our @RULES;

sub define {
    my ($class, $name, %opt) = @_;
    die "MT::Partition::define: name required\n" unless defined $name && length $name;
    die "MT::Partition::define: partition '$name' already exists\n"
        if exists $PARTITIONS{$name};

    my $ns  = $opt{namespace};
    my $cls = $opt{class};
    my $workers = defined $opt{workers} ? $opt{workers} : 0;
    my $pool;
    if ($opt{pool}) {
        $pool = $opt{pool};
    } elsif ($workers > 0) {
        $pool = MT::Pool->new(
            workers => $workers,
            ($opt{queue} ? (queue => $opt{queue}) : ()),
        );
    }

    my $self = bless {
        name           => $name,
        namespace      => $ns,
        class          => $cls,
        pool           => $pool,
        owns_pool      => ($opt{pool} ? 0 : ($pool ? 1 : 0)),
        workers        => $workers,
        state          => 'active',
        meta           => { %{ $opt{meta} || {} } },
        jobs_submitted => 0,
        jobs_completed => 0,
        jobs_failed    => 0,
    }, $class;

    $PARTITIONS{$name} = $self;
    return $self;
}

sub bind {
    my ($class, $name) = @_;
    die "MT::Partition::bind: unknown partition '$name'\n"
        unless defined $name && exists $PARTITIONS{$name};
    return $PARTITIONS{$name};
}

sub exists {
    my ($class, $name) = @_;
    return (defined $name && exists $PARTITIONS{$name}) ? 1 : 0;
}

sub list {
    return sort keys %PARTITIONS;
}

sub remove {
    my ($class, $name) = @_;
    my $p = delete $PARTITIONS{$name} or return 0;
    if ($p->{owns_pool} && $p->{pool}) {
        $p->{pool}->shutdown;
        $p->{pool} = undef;
    }
    $p->{state} = 'removed';
    return 1;
}

sub clear_all {
    my ($class) = @_;
    for my $name (keys %PARTITIONS) {
        $class->remove($name);
    }
    @RULES = ();
    return 1;
}

# Policy assignment rules: match by group, job_type, or name with priority.
sub add_rule {
    my ($class, %rule) = @_;
    my $target = $rule{partition}
        // die "MT::Partition::add_rule: partition target required\n";
    push @RULES, {
        partition   => $target,
        priority    => defined $rule{priority} ? $rule{priority} : 0,
        match_group => $rule{match_group},
        match_job   => $rule{match_job},
        match_name  => $rule{match_name},
        default     => $rule{default} ? 1 : 0,
    };
    @RULES = sort { $b->{priority} <=> $a->{priority} } @RULES;
    return scalar @RULES;
}

sub rules {
    return [ map { { %$_ } } @RULES ];
}

sub _matches {
    my ($pat, $val) = @_;
    return 1 unless defined $pat;
    return 0 unless defined $val;
    if (ref $pat eq 'Regexp') {
        return $val =~ $pat ? 1 : 0;
    }
    return $val eq $pat ? 1 : 0;
}

sub resolve {
    my ($class, %ctx) = @_;
    my $default_target;
    for my $r (@RULES) {
        if ($r->{default}) {
            $default_target //= $r->{partition};
            next;
        }
        next unless _matches($r->{match_group}, $ctx{group});
        next unless _matches($r->{match_job},   $ctx{job});
        next unless _matches($r->{match_name},  $ctx{name});
        return $class->bind($r->{partition})
            if exists $PARTITIONS{ $r->{partition} };
    }
    if (defined $default_target && exists $PARTITIONS{$default_target}) {
        return $class->bind($default_target);
    }
    die "MT::Partition::resolve: no matching partition rule\n";
}

sub dispatch {
    my ($class, %req) = @_;
    my $part = defined $req{partition}
        ? $class->bind($req{partition})
        : $class->resolve(
            group => $req{group},
            job   => $req{job},
            name  => $req{name},
        );
    my $code = $req{code} // die "MT::Partition::dispatch: code required\n";
    my @args = @{ $req{args} || [] };
    return $part->run($code, @args);
}

sub name      { $_[0]->{name} }
sub namespace { $_[0]->{namespace} }
sub class     { $_[0]->{class} }
sub pool      { $_[0]->{pool} }
sub state     { $_[0]->{state} }

sub run {
    my ($self, $code, @args) = @_;
    die "MT::Partition::run: partition '$self->{name}' is not active\n"
        unless $self->{state} eq 'active';
    $self->{jobs_submitted}++;

    if ($self->{class}) {
        $self->{class}->bind_thread;
        $self->{class}->charge_cpu(1);
    }

    my $res;
    my $ok = eval {
        if ($self->{pool}) {
            my $job = $self->{pool}->run($code, @args);
            $res = $job->join;
        } elsif ($self->{namespace}) {
            $res = $self->{namespace}->run(sub {
                my ($ns, @a) = @_;
                return $code->(@a);
            }, @args);
        } else {
            $res = $code->(@args);
        }
        1;
    };
    if (!$ok) {
        $self->{jobs_failed}++;
        die $@;
    }
    $self->{jobs_completed}++;
    return $res;
}

sub map {
    my ($self, $code, @items) = @_;
    die "MT::Partition::map: partition '$self->{name}' is not active\n"
        unless $self->{state} eq 'active';
    $self->{jobs_submitted} += scalar @items;
    if ($self->{class}) {
        $self->{class}->bind_thread;
        $self->{class}->charge_cpu(scalar @items || 1);
    }
    my @out;
    my $ok = eval {
        if ($self->{pool}) {
            @out = $self->{pool}->map($code, @items);
        } elsif ($self->{namespace}) {
            @out = $self->{namespace}->run(sub {
                my ($ns) = @_;
                return map { $code->($_) } @items;
            });
        } else {
            @out = map { $code->($_) } @items;
        }
        1;
    };
    if (!$ok) {
        $self->{jobs_failed} += scalar @items;
        die $@;
    }
    $self->{jobs_completed} += scalar @items;
    return @out;
}

sub status {
    my ($inv, $name) = @_;
    if (ref $inv) {
        return {
            name           => $inv->{name},
            state          => $inv->{state},
            workers        => $inv->{workers},
            has_pool       => $inv->{pool} ? 1 : 0,
            namespace      => $inv->{namespace} ? $inv->{namespace}->name : undef,
            class          => $inv->{class} ? $inv->{class}->name : undef,
            class_stats    => $inv->{class} ? $inv->{class}->stats : undef,
            jobs_submitted => $inv->{jobs_submitted},
            jobs_completed => $inv->{jobs_completed},
            jobs_failed    => $inv->{jobs_failed},
            meta           => { %{ $inv->{meta} } },
        };
    }
    if (defined $name) {
        return $inv->bind($name)->status;
    }
    return {
        partitions => { map { $_ => $PARTITIONS{$_}->status } keys %PARTITIONS },
        rules      => $inv->rules,
    };
}

sub shutdown {
    my ($self) = @_;
    return 1 if $self->{state} ne 'active';
    if ($self->{owns_pool} && $self->{pool}) {
        $self->{pool}->shutdown;
        $self->{pool} = undef;
    }
    $self->{state} = 'stopped';
    return 1;
}

1;

__END__

=head1 NAME

MT::Partition - policy layer and named partitions over MT::Pool, MT::Namespace, and MT::Class

=cut

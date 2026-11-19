package MT::Namespace;

use strict;
use warnings;
use File::Spec;

BEGIN {
    require MT unless defined &MT::Namespace::_create;
}

our %CAP_BITS = (
    read_fs   => 1 << 0,
    write_fs  => 1 << 1,
    net       => 1 << 2,
    exec      => 1 << 3,
    ipc       => 1 << 4,
    admin     => 1 << 5,
);

sub capabilities {
    return MT::Namespace::_caps();
}

sub fault_model {
    return MT::Namespace::_caps()->{fault_model};
}

sub _caps_to_mask {
    my ($caps) = @_;
    return 0 unless $caps;
    return $caps if !ref $caps && $caps =~ /^\d+$/;
    my $mask = 0;
    for my $c (@$caps) {
        $mask |= ($CAP_BITS{$c} // 0);
    }
    return $mask;
}

sub new {
    my ($class, %opt) = @_;
    my $name       = $opt{name}  // 'ns';
    my $cwd        = $opt{cwd}   // '';
    my $umask_val  = defined $opt{umask} ? $opt{umask} : 022;
    my $cap_mask   = _caps_to_mask($opt{caps});
    my $id         = MT::Namespace::_create($name, $cwd, $umask_val, $cap_mask);
    die "MT::Namespace::new: failed to allocate namespace slot\n" unless $id;

    my $info = MT::Namespace::_info($id);
    my $self = bless {
        id          => $id,
        name        => $name,
        cwd         => $info->{cwd},
        umask       => $umask_val,
        cap_mask    => $cap_mask,
        caps        => { map { $_ => 1 } @{ $opt{caps} || [] } },
        env         => { %{ $opt{env} || {} } },
        clean_env   => $opt{clean_env} ? 1 : 0,
        isolate_os  => $opt{isolate_os} ? 1 : 0,
        packages    => {},
        fhs         => {},
        next_fd     => 3,
    }, $class;

    if ($opt{packages} && ref $opt{packages} eq 'HASH') {
        for my $pkg (keys %{ $opt{packages} }) {
            $self->define_package($pkg, $opt{packages}{$pkg});
        }
    }
    return $self;
}

sub id          { $_[0]->{id} }
sub name        { $_[0]->{name} }
sub cwd         { $_[0]->{cwd} }
sub umask_val   { $_[0]->{umask} }
sub info        { MT::Namespace::_info($_[0]->{id}) }
sub current_id  { MT::Namespace::_current() }

# Define overlay symbols for a package within this namespace without
# colliding with other namespaces in the same process.
sub define_package {
    my ($self, $pkg, $subs) = @_;
    die "MT::Namespace::define_package: subs must be hashref\n"
        unless ref $subs eq 'HASH';
    $self->{packages}{$pkg} ||= {};
    for my $sym (keys %$subs) {
        $self->{packages}{$pkg}{$sym} = $subs->{$sym};
    }
    return 1;
}

# Resolve a relative path against this namespace's logical cwd.
sub resolve_path {
    my ($self, $path) = @_;
    return $self->{cwd} unless defined $path && length $path;
    return $path if File::Spec->file_name_is_absolute($path);
    return File::Spec->catfile($self->{cwd}, $path);
}

# Capability checks
sub has_cap {
    my ($self, $cap) = @_;
    return $self->{caps}{$cap} ? 1 : 0;
}

sub grant_cap {
    my ($self, $cap) = @_;
    $self->{caps}{$cap} = 1;
    $self->{cap_mask} |= ($CAP_BITS{$cap} // 0);
    return 1;
}

sub revoke_cap {
    my ($self, $cap) = @_;
    delete $self->{caps}{$cap};
    $self->{cap_mask} &= ~($CAP_BITS{$cap} // 0);
    return 1;
}

sub caps {
    my ($self) = @_;
    return [ sort keys %{ $self->{caps} } ];
}

# Virtual per-namespace filehandle table metadata
sub register_fh {
    my ($self, $label, $mode, $path) = @_;
    my $fd = $self->{next_fd}++;
    $self->{fhs}{$fd} = {
        fd    => $fd,
        label => $label,
        mode  => $mode // 'r',
        path  => defined $path ? $self->resolve_path($path) : '',
    };
    return $fd;
}

sub close_fh {
    my ($self, $fd) = @_;
    return delete $self->{fhs}{$fd} ? 1 : 0;
}

sub list_fhs {
    my ($self) = @_;
    return [ map { $self->{fhs}{$_} } sort { $a <=> $b } keys %{ $self->{fhs} } ];
}

# Execute a coderef inside this namespace's %ENV overlay, package overlay,
# and thread-local namespace context.
sub run {
    my ($self, $code, @args) = @_;
    die "MT::Namespace::run: expected coderef\n" unless ref $code eq 'CODE';

    my $rc = MT::Namespace::_enter($self->{id}, $self->{isolate_os});
    # On macOS/AIX/z/OS, isolate_os returns PERL_KRT_ENOSYS (-38); gracefully
    # continue with overlay isolation.
    if ($rc < 0 && $rc != -38) {
        die "MT::Namespace::run: enter failed ($rc)\n";
    }

    my %saved_env = %ENV;
    my @saved_syms;

    eval {
        if ($self->{clean_env}) {
            %ENV = %{ $self->{env} };
        } else {
            for my $k (keys %{ $self->{env} }) {
                $ENV{$k} = $self->{env}{$k};
            }
        }

        no strict 'refs';
        no warnings 'redefine';
        for my $pkg (keys %{ $self->{packages} }) {
            for my $sym (keys %{ $self->{packages}{$pkg} }) {
                my $fq = "${pkg}::${sym}";
                my $prev = defined(&{$fq}) ? \&{$fq} : undef;
                push @saved_syms, [ $fq, $prev ];
                *{$fq} = $self->{packages}{$pkg}{$sym};
            }
        }
    };
    if ($@) {
        %ENV = %saved_env;
        MT::Namespace::_leave();
        die $@;
    }

    my ($want, @res, $res, $err);
    $want = wantarray;
    eval {
        if ($want) {
            @res = $code->($self, @args);
        } else {
            $res = $code->($self, @args);
        }
        1;
    } or $err = $@;

    {
        no strict 'refs';
        no warnings 'redefine';
        for my $entry (reverse @saved_syms) {
            my ($fq, $prev) = @$entry;
            if (defined $prev) {
                *{$fq} = $prev;
            } else {
                undef *{$fq};
            }
        }
    }
    %ENV = %saved_env;
    MT::Namespace::_leave();

    die $err if defined $err && length $err;
    return $want ? @res : $res;
}

sub destroy {
    my ($self) = @_;
    return 0 unless defined $self->{id};
    my $rc = MT::Namespace::_destroy($self->{id});
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

MT::Namespace - worker-group isolated overlay namespaces (WPAR tier)

=head1 DESCRIPTION

Provides per-group overlay namespaces with isolated C<%ENV>, package symbol
overlays, logical C<cwd>, umask, capability sets, and virtual filehandle
tables.  On Linux, C<isolate_os =E<gt> 1> additionally unshares C<CLONE_FS>
and C<CLONE_FILES>; on macOS, AIX, and z/OS this is documented as unavailable
and overlay isolation is used.  Fault model: C<"shared interpreter is the kernel">.

=cut

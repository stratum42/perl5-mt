package MT::SharedImage;

use strict;
use warnings;

BEGIN {
    require MT unless defined &MT::SharedImage::_create;
}

# Compute a deterministic manifest string from a list of module names (or
# name => version/path pairs) so image invalidation detects version or file
# content changes.
sub manifest_for {
    my ($class, $modules) = @_;
    return '' unless $modules;
    if (ref $modules eq 'HASH') {
        return join(',', map {
            my $k = $_;
            my $v = defined $modules->{$k} ? $modules->{$k} : 'undef';
            "$k=$v";
        } sort keys %$modules);
    }
    if (ref $modules eq 'ARRAY') {
        my @parts;
        for my $mod (sort @$modules) {
            (my $pm = "$mod.pm") =~ s{::}{/}g;
            my $ver = eval { no strict 'refs'; ${"${mod}::VERSION"} };
            $ver = defined $ver ? "$ver" : '0';
            my $path = $INC{$pm} || '';
            my $mtime = ($path && -f $path) ? (stat($path))[9] : 0;
            my $size  = ($path && -f $path) ? (stat($path))[7] : 0;
            push @parts, "$mod=$ver:$size:$mtime";
        }
        return join(',', @parts);
    }
    return "$modules";
}

sub build_id {
    return MT::SharedImage::_build_id();
}

# Persist a frozen image file mapped at a fixed address with header magic,
# version, build_id, module manifest hash, and payload checksum.
sub create {
    my ($class, %opt) = @_;
    my $path = $opt{path} // die "MT::SharedImage::create: path required\n";
    my $fixed_addr = $opt{fixed_addr} // 0;
    my $manifest = defined $opt{manifest}
        ? (ref $opt{manifest} ? $class->manifest_for($opt{manifest}) : $opt{manifest})
        : ($opt{modules} ? $class->manifest_for($opt{modules}) : '');
    my $payload = defined $opt{payload} ? $opt{payload} : '';

    my $info = MT::SharedImage::_create($path, $fixed_addr, $manifest, $payload);
    return bless {
        id   => $info->{id},
        path => $path,
        info => $info,
    }, $class;
}

# Attach to an existing persistent shared image file.  Validates magic,
# version, perl build_id, module manifest, and payload checksum; falls back
# cleanly when the fixed address is unavailable or force_fallback is set.
sub attach {
    my ($class, %opt) = @_;
    my $path = $opt{path} // die "MT::SharedImage::attach: path required\n";
    my $manifest = defined $opt{manifest}
        ? (ref $opt{manifest} ? $class->manifest_for($opt{manifest}) : $opt{manifest})
        : ($opt{modules} ? $class->manifest_for($opt{modules}) : '');
    my $force_fallback = $opt{force_fallback} ? 1 : 0;

    my $info = MT::SharedImage::_attach($path, $manifest, $force_fallback);
    if ($info->{fallback} && $opt{on_fallback}) {
        $opt{on_fallback}->($info);
    }
    return bless {
        id   => $info->{id},
        path => $path,
        info => $info,
    }, $class;
}

sub validate {
    my ($class, %opt) = @_;
    my $path = $opt{path} // die "MT::SharedImage::validate: path required\n";
    my $manifest = defined $opt{manifest}
        ? (ref $opt{manifest} ? $class->manifest_for($opt{manifest}) : $opt{manifest})
        : ($opt{modules} ? $class->manifest_for($opt{modules}) : '');
    return MT::SharedImage::_validate($path, $manifest);
}

sub id      { $_[0]->{id} }
sub path    { $_[0]->{path} }
sub info    { MT::SharedImage::_info($_[0]->{id}) }
sub payload { MT::SharedImage::_payload($_[0]->{id}) }

sub detach {
    my ($self) = @_;
    return 0 unless defined $self->{id};
    my $rc = MT::SharedImage::_detach($self->{id});
    $self->{id} = undef;
    return $rc == 0 ? 1 : 0;
}

sub DESTROY {
    my ($self) = @_;
    return if MT::Worker::is_worker();
    $self->detach if defined $self->{id};
}

1;

__END__

=head1 NAME

MT::SharedImage - persistent fixed-address shared image file (LPAR tier)

=head1 SYNOPSIS

    use MT::SharedImage;
    my $img = MT::SharedImage->create(
        path    => '/tmp/app.mtimg',
        modules => [qw(strict warnings)],
        payload => $frozen_blob,
    );
    my $attached = MT::SharedImage->attach(
        path    => '/tmp/app.mtimg',
        modules => [qw(strict warnings)],
    );

=cut

package MT::PDLBridge;

use strict;
use warnings;
use Digest::SHA qw(sha256_hex);

BEGIN {
    require MT unless defined &MT::PDLBridge::_ndarray_create;
}

use constant {
    DTYPE_F64 => 1,
    DTYPE_I64 => 2,
};

# Report whether CPAN PDL is available in this environment.
sub has_pdl {
    return eval { require PDL; 1 } ? 1 : 0;
}

# Marker reporting that MT::PDLBridge conforms to PERL_XS_THREADSAFE
# semantics (no shared mutable globals; shm-backed capability handles).
sub is_threadsafe { 1 }

# Per-interpreter audit summary for PDL::Core threadsafety migration.
sub audit_report {
    return {
        xs_threadsafe_marker => 'PERL_XS_THREADSAFE',
        pdl_installed        => has_pdl(),
        compartment_breaks   => [
            'PDL::Core exports raw C struct pointers (Core *PDL) via IV scalars that cannot cross compartment address spaces',
            'Direct SvPVX data-pointer sharing across interpreters violates compartment VM isolation',
        ],
        per_interpreter_audit => [
            {
                symbol => 'PDL::Core::SHARE (Core *PDL vtable)',
                issue  => 'Process-global mutable vtable pointer',
                fix    => 'Freeze vtable read-only after boot or store per-interpreter via MY_CXT',
            },
            {
                symbol => 'PDL_BVAL_* / pdl_bval_nan (bad-value globals)',
                issue  => 'Mutable C globals raced by concurrent workers',
                fix    => 'Move bad-value table into per-interpreter MY_CXT or seal immutable',
            },
            {
                symbol => 'ndarray data buffer',
                issue  => 'Private malloc buffer non-shareable across workers',
                fix    => 'Back ndarray storage with sealed krt_shm capability handles',
            },
        ],
    };
}

# Allocate a shm-backed ndarray capability handle.
sub create_array {
    my ($class, %opt) = @_;
    my $nelem = $opt{nelem} // 0;
    my $dims  = $opt{dims};
    if (!$nelem && $dims && ref $dims eq 'ARRAY') {
        $nelem = 1;
        $nelem *= $_ for @$dims;
    }
    die "MT::PDLBridge::create_array: nelem must be > 0\n" unless $nelem > 0;
    my $dtype = (defined $opt{dtype} && $opt{dtype} eq 'i64') ? DTYPE_I64 : DTYPE_F64;
    my $id = MT::PDLBridge::_ndarray_create($dtype, $nelem, $dims);
    die "MT::PDLBridge::create_array: shm allocation failed\n" unless $id;
    return MT::PDLBridge::Array->_wrap($id, 1);
}

# Attach to an existing shm-backed ndarray by integer capability ID (safe to
# call inside any MT::Pool worker).
sub attach_array {
    my ($class, $id) = @_;
    die "MT::PDLBridge::attach_array: id required\n" unless defined $id && $id > 0;
    return MT::PDLBridge::Array->_wrap($id, 0);
}

# Auto-bridge from a CPAN PDL object into a shm-backed ndarray when PDL is installed.
sub from_pdl {
    my ($class, $pdl) = @_;
    die "MT::PDLBridge::from_pdl: CPAN PDL is not installed\n" unless has_pdl();
    my $flat = $pdl->double->flat;
    my $n    = $flat->nelem;
    my @dims = $pdl->dims;
    my $arr  = $class->create_array(nelem => $n, dims => \@dims, dtype => 'f64');
    my @vals = $flat->list;
    for my $i (0 .. $#vals) {
        $arr->set($i, $vals[$i]);
    }
    return $arr;
}

# Auto-bridge back to a CPAN PDL object when PDL is installed.
sub to_pdl {
    my ($class, $arr) = @_;
    die "MT::PDLBridge::to_pdl: CPAN PDL is not installed\n" unless has_pdl();
    my $info = $arr->info;
    my $n    = $info->{nelem};
    my @vals = map { $arr->get($_) } 0 .. ($n - 1);
    my $pdl  = PDL::double(\@vals);
    my @dims = @{ $info->{dims} || [$n] };
    return @dims > 1 ? $pdl->reshape(@dims) : $pdl;
}

# Deterministic scalar reference kernel matching C Perl_mt_ndarray_kernel_slice.
sub scalar_reference_kernel {
    my ($start, $step, $n, $alpha, $beta) = @_;
    my @out;
    my $sum = 0.0;
    my $c   = 0.0;
    my $bytes = '';
    for my $i (0 .. $n - 1) {
        my $x  = $start + $i * $step;
        my $t1 = $x * $alpha;
        my $t2 = $t1 + $beta;
        my $t3 = $x + 0.5;
        my $y  = $t2 * $t3;
        $bytes .= pack('d>', $y);
        my $t = $sum + $y;
        if (abs($sum) >= abs($y)) {
            $c += ($sum - $t) + $y;
        } else {
            $c += ($y - $t) + $sum;
        }
        $sum = $t;
    }
    return {
        digest => sha256_hex($bytes),
        sum    => $sum + $c,
        nelem  => $n,
    };
}

# Worker-callable slice entry point (named sub in frozen boot image).
sub worker_slice_kernel {
    my ($src_id, $dst_id, $start, $len, $alpha, $beta) = @_;
    my $rc = MT::PDLBridge::_ndarray_kernel_slice(
        $src_id, $dst_id, $start, $len, $alpha, $beta
    );
    die "MT::PDLBridge::worker_slice_kernel failed ($rc)\n" if $rc < 0;
    my $bytes  = MT::PDLBridge::_ndarray_slice_be_bytes($dst_id, $start, $len);
    my $digest = sha256_hex($bytes);
    my $sum    = MT::PDLBridge::_ndarray_sum_slice($dst_id, $start, $len);
    return join('|', $start, $len, $digest, unpack('H16', pack('d>', $sum)));
}

package MT::PDLBridge::Array;

use strict;
use warnings;
use Digest::SHA qw(sha256_hex);

sub _wrap {
    my ($class, $id, $owned) = @_;
    return bless { id => $id, owned => $owned ? 1 : 0 }, $class;
}

sub id        { $_[0]->{id} }
sub info      { MT::PDLBridge::_ndarray_info($_[0]->{id}) }
sub is_sealed { MT::PDLBridge::_ndarray_is_sealed($_[0]->{id}) }

sub seal {
    my ($self) = @_;
    my $rc = MT::PDLBridge::_ndarray_seal($self->{id});
    die "MT::PDLBridge::Array::seal failed ($rc)\n" if $rc < 0;
    return 1;
}

sub set {
    my ($self, $idx, $val) = @_;
    my $rc = MT::PDLBridge::_ndarray_set_f64($self->{id}, $idx, $val);
    die "MT::PDLBridge::Array::set failed ($rc)\n" if $rc < 0;
    return 1;
}

sub get {
    my ($self, $idx) = @_;
    return MT::PDLBridge::_ndarray_get_f64($self->{id}, $idx);
}

sub fill_seq {
    my ($self, $start, $step) = @_;
    my $rc = MT::PDLBridge::_ndarray_fill_seq($self->{id}, $start, $step);
    die "MT::PDLBridge::Array::fill_seq failed ($rc)\n" if $rc < 0;
    return 1;
}

sub sum_slice {
    my ($self, $start, $len) = @_;
    return MT::PDLBridge::_ndarray_sum_slice($self->{id}, $start, $len);
}

sub dot_slice {
    my ($self, $other, $start, $len) = @_;
    my $oid = ref $other ? $other->id : $other;
    return MT::PDLBridge::_ndarray_dot_slice($self->{id}, $oid, $start, $len);
}

sub slice_digest {
    my ($self, $start, $len) = @_;
    my $bytes = MT::PDLBridge::_ndarray_slice_be_bytes($self->{id}, $start, $len);
    return sha256_hex($bytes);
}

sub digest {
    my ($self) = @_;
    my $info = $self->info;
    return $self->slice_digest(0, $info->{nelem});
}

sub destroy {
    my ($self) = @_;
    return 0 unless defined $self->{id} && $self->{owned};
    my $rc = MT::PDLBridge::_ndarray_destroy($self->{id});
    $self->{id} = undef;
    return $rc == 0 ? 1 : 0;
}

sub DESTROY {
    my ($self) = @_;
    return if MT::Worker::is_worker();
    $self->destroy if defined $self->{id} && $self->{owned};
}

1;

__END__

=head1 NAME

MT::PDLBridge - PERL_XS_THREADSAFE shm-backed ndarray bridge and PDL proof

=cut

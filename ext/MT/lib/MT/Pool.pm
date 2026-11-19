package MT::Pool;

use strict;
use warnings;
BEGIN {
    require MT unless defined &MT::Pool::_encode_list;
}
use MT::Channel;
use MT::SharedHash;
use MT::Fault;
use MT::Checkpoint;
use Time::HiRes ();
# B is loaded here, before any freeze: after it, the XS modules are loaded
# into compartments, and B does not load there.
use B ();

# A supervised pool of workers that take jobs from a shared channel.
# Supports:
#   - Nested data & capability marshaling
#   - Worker fault detection, exponential-backoff respawn with cap, in-flight
#     job failure reporting, and ->faults inspection
#   - Process-wide named pools: MT::Pool->new(name => ...) / MT::Pool->named(...)
#   - NonStop-style process pairs (pairs => 1) with warm backup workers and
#     MT::Checkpoint takeover on primary fault

our @LIVE;
our %NAMED_POOLS;

sub _pack {
    return join '', map { my $f = defined $_ ? "$_" : ''; length($f) . ':' . $f } @_;
}

sub _unpack {
    my ($s) = @_;
    my @f;
    while (length $s) {
        $s =~ /\A(\d+):/ or die "MT::Pool: corrupt message\n";
        my $n = $1;
        my $start = length($1) + 1;
        push @f, substr($s, $start, $n);
        $s = substr($s, $start + $n);
    }
    return @f;
}

sub _sub_name {
    my ($code) = @_;
    die "MT::Pool: the code must be a reference to a named sub\n"
        unless ref $code eq 'CODE';
    my $gv = B::svref_2object($code)->GV;
    my $name = $gv->NAME;
    die "MT::Pool: the code must be a named sub of the image, not an anonymous one\n"
        if $name eq '__ANON__';
    return $gv->STASH->NAME . '::' . $name;
}

sub _check_args {
    for (@_) {
        die "MT::Pool: arguments must be plain values, not references\n"
            if ref $_;
    }
}

# Primary worker loop. Runs in each worker thread.
sub _service {
    my ($jobs_id, $inflight_id, $ckpt_id, $pair_id) = @_;
    my $jobs     = MT::Channel->attach($jobs_id);
    my $inflight = $inflight_id ? MT::SharedHash->attach($inflight_id) : undef;
    my $ckpt     = $ckpt_id     ? MT::SharedHash->attach($ckpt_id)     : undef;
    my $wid      = MT::Worker::id();

    while (defined(my $msg = $jobs->recv)) {
        my ($reply_id, $name, $index, @args) = _decode_list($msg);
        last if $name eq '';

        $inflight->set($wid, $msg) if $inflight;
        if ($ckpt) {
            $ckpt->delete("p:$pair_id:blob");
            $ckpt->delete("p:$pair_id:seq");
            MT::Checkpoint::_set_context(
                pair_id   => $pair_id,
                store     => $ckpt,
                is_backup => 0,
                seq       => 0,
            );
        }

        my $reply = MT::Channel->attach($reply_id);
        my $r = eval { no strict 'refs'; &{$name}(@args) };
        my $err = $@;

        MT::Checkpoint::_clear_context() if $ckpt;
        $inflight->delete($wid) if $inflight;

        if ($err) {
            $reply->send(_encode_list($index, 'E', "$err"));
        }
        else {
            $reply->send(_encode_list($index, 'O', $r));
        }
    }
    return 1;
}

# Designated backup worker loop for pairs => 1.
# Waits warm on its pair's backup channel and resumes the primary's in-flight
# job from the last committed MT::Checkpoint upon primary fault.
sub _backup_service {
    my ($backup_ch_id, $ckpt_id, $pair_id) = @_;
    my $bch  = MT::Channel->attach($backup_ch_id);
    my $ckpt = MT::SharedHash->attach($ckpt_id);

    while (defined(my $msg = $bch->recv)) {
        my ($reply_id, $name, $index, @args) = _decode_list($msg);
        last if $name eq '';

        MT::Checkpoint::_set_context(
            pair_id   => $pair_id,
            store     => $ckpt,
            is_backup => 1,
        );

        my $reply = MT::Channel->attach($reply_id);
        my $r = eval { no strict 'refs'; &{$name}(@args) };
        my $err = $@;

        MT::Checkpoint::_clear_context();

        if ($err) {
            $reply->send(_encode_list($index, 'E', "$err"));
        }
        else {
            $reply->send(_encode_list($index, 'O', $r));
        }
    }
    return 1;
}

sub new {
    my ($class, %opt) = @_;
    my $n = defined $opt{workers} ? $opt{workers} : 1;
    die "MT::Pool: workers must be at least 1\n" if $n < 1;

    my $queue_cap      = $opt{queue}           || 1024;
    my $pairs          = $opt{pairs}           ? 1 : 0;
    my $max_restarts   = defined $opt{max_restarts}   ? $opt{max_restarts}   : 32;
    my $backoff_base   = defined $opt{backoff_base_ms}? $opt{backoff_base_ms}: 5;
    my $backoff_max    = defined $opt{backoff_max_ms} ? $opt{backoff_max_ms} : 500;
    my $backoff_reset  = defined $opt{backoff_reset_ms}? $opt{backoff_reset_ms}: 2000;
    my $name           = $opt{name};

    MT::Image::freeze() unless MT::Image::frozen();

    my $jobs     = MT::Channel->new($queue_cap);
    my $inflight = MT::SharedHash->new();
    my $ckpt     = $pairs ? MT::SharedHash->new() : undef;

    my @slots;
    for my $idx (0 .. $n - 1) {
        my $wid = MT::Worker::spawn(
            'MT::Pool::_service',
            $jobs->id, $inflight->id, ($ckpt ? $ckpt->id : 0), $idx,
        );
        my %slot = (
            idx          => $idx,
            wid          => $wid,
            restarts     => 0,
            backoff_exp  => 0,
            last_restart => 0,
            respawn_at   => 0,
            dead_perm    => 0,
        );
        if ($pairs) {
            my $bch = MT::Channel->new(16);
            my $bwid = MT::Worker::spawn(
                'MT::Pool::_backup_service',
                $bch->id, $ckpt->id, $idx,
            );
            $slot{backup_ch}  = $bch;
            $slot{backup_wid} = $bwid;
        }
        push @slots, \%slot;
    }

    my $self = bless {
        jobs             => $jobs,
        queue_cap        => $queue_cap,
        inflight         => $inflight,
        ckpt             => $ckpt,
        pairs            => $pairs,
        slots            => \@slots,
        live             => 1,
        client           => 0,
        name             => $name,
        on_fault         => $opt{on_fault},
        max_restarts     => $max_restarts,
        backoff_base_ms  => $backoff_base,
        backoff_max_ms   => $backoff_max,
        backoff_reset_ms => $backoff_reset,
        faults           => [],
        restarts         => 0,
        takeovers        => 0,
        last_takeover_ms => 0,
    }, $class;

    if (defined $name && length $name) {
        my ($rc) = MT::Pool::_named_register($name, $jobs->id, 1);
        die "MT::Pool: failed to register name '$name'\n" if $rc < 0;
        $NAMED_POOLS{$name} = $self;
    }

    push @LIVE, $self;
    return $self;
}

# Bind to a named pool (from main or from any worker).
sub named {
    my ($class, $name) = @_;
    die "MT::Pool->named: name must be a non-empty string\n"
        unless defined $name && length $name;
    if (!MT::Worker::is_worker() && exists $NAMED_POOLS{$name} && $NAMED_POOLS{$name}{live}) {
        return $NAMED_POOLS{$name};
    }
    my $jobs_id = MT::Pool::_named_lookup($name);
    return undef unless defined $jobs_id;
    return bless {
        jobs      => MT::Channel->attach($jobs_id),
        queue_cap => 1024,
        live      => 1,
        client    => 1,
        name      => $name,
        faults    => [],
    }, $class;
}

sub name            { $_[0]{name} }
sub restarts        { $_[0]{restarts} // 0 }
sub takeovers       { $_[0]{takeovers} // 0 }
sub last_takeover_ms{ $_[0]{last_takeover_ms} // 0 }

sub faults {
    my ($self) = @_;
    $self->supervise() if $self->{live} && !$self->{client};
    my $list = $self->{faults} || [];
    return wantarray ? @$list : [ @$list ];
}

sub workers_alive {
    my ($self) = @_;
    return 0 if $self->{client} || !$self->{slots};
    return scalar grep { defined $_->{wid} && !$_->{dead_perm} } @{ $self->{slots} };
}

# Scan worker slots for unexpected deaths, reap faulted workers, fail or
# hand over in-flight jobs, and respawn with exponential backoff & cap.
sub supervise {
    my ($self) = @_;
    return 0 if !$self->{live} || $self->{client} || !$self->{slots};

    my $now = Time::HiRes::time();
    my $events = 0;

    for my $slot (@{ $self->{slots} }) {
        next if $slot->{dead_perm};

        if (defined $slot->{wid}) {
            my $wid = $slot->{wid};
            my $st  = MT::Worker::state($wid);
            if ($st != 0) {
                $events++;
                my $t0  = Time::HiRes::time();
                my $sig = MT::Worker::faulted_sig($wid);
                my $ok  = eval { MT::Worker::join($wid); 1 };
                my $err = $ok ? '' : "$@";
                MT::Fault::poll();
                my $last_f = MT::Fault::last();

                my %rec = (
                    worker_id  => $wid,
                    slot       => $slot->{idx},
                    signal     => $sig,
                    error      => $err || "worker $wid exited unexpectedly",
                    timestamp  => $t0,
                    restarts   => $slot->{restarts} + 1,
                    paired     => $self->{pairs},
                    takeover   => 0,
                    exhausted  => 0,
                );
                if ($last_f && $last_f->{unit_kind} eq 'worker' && $last_f->{unit_id} == $wid) {
                    $rec{seq}      = $last_f->{seq};
                    $rec{cause}    = $last_f->{cause};
                    $rec{severity} = $last_f->{severity};
                    $rec{message}  = $last_f->{message};
                }
                else {
                    $rec{cause}    = $sig ? 'SIGNAL' : 'WORKER_EXIT';
                    $rec{severity} = 'recoverable';
                    $rec{message}  = $rec{error};
                }

                $slot->{wid} = undef;

                # Handle any job that was in-flight on this worker
                my $in_msg = $self->{inflight}->get($wid);
                if (defined $in_msg) {
                    $self->{inflight}->delete($wid);
                    if ($self->{pairs} && $slot->{backup_ch}) {
                        $slot->{backup_ch}->send($in_msg);
                        $self->{takeovers}++;
                        $self->{last_takeover_ms} = (Time::HiRes::time() - $t0) * 1000.0;
                        $rec{takeover} = 1;
                        $rec{takeover_ms} = $self->{last_takeover_ms};
                    }
                    else {
                        my ($reply_id, $name, $index) = _decode_list($in_msg);
                        if ($reply_id) {
                            my $reply = MT::Channel->attach($reply_id);
                            $reply->send(_encode_list($index, 'F', \%rec));
                        }
                    }
                }

                # Exponential backoff and restart cap
                if ($slot->{restarts} >= $self->{max_restarts}) {
                    $slot->{dead_perm} = 1;
                    $rec{exhausted} = 1;
                }
                else {
                    $slot->{restarts}++;
                    if ($slot->{last_restart} > 0
                        && ($now - $slot->{last_restart}) * 1000.0 > $self->{backoff_reset_ms}) {
                        $slot->{backoff_exp} = 0;
                    }
                    my $exp = $slot->{backoff_exp};
                    $exp = 10 if $exp > 10;
                    my $delay_ms = $self->{backoff_base_ms} * (2 ** $exp);
                    $delay_ms = $self->{backoff_max_ms} if $delay_ms > $self->{backoff_max_ms};
                    $slot->{backoff_exp}++;
                    $slot->{last_restart} = $now;
                    $slot->{respawn_at}   = $now + ($delay_ms / 1000.0);
                    $rec{backoff_ms}      = $delay_ms;
                    $self->{restarts}++;
                }

                push @{ $self->{faults} }, \%rec;
            }
        }

        if (!defined $slot->{wid} && !$slot->{dead_perm} && $now >= ($slot->{respawn_at} // 0)) {
            my $new_wid = MT::Worker::spawn(
                'MT::Pool::_service',
                $self->{jobs}->id,
                $self->{inflight}->id,
                ($self->{ckpt} ? $self->{ckpt}->id : 0),
                $slot->{idx},
            );
            $slot->{wid} = $new_wid;
        }
    }

    return $events;
}

sub _all_slots_exhausted {
    my ($self) = @_;
    return 0 if $self->{client} || !$self->{slots};
    for my $s (@{ $self->{slots} }) {
        return 0 unless $s->{dead_perm};
    }
    return 1;
}

# Starts $code (a named sub of the image) with @args on a worker.  Returns a
# job: ->join waits for the result, or rethrows the error.
sub run {
    my ($self, $code, @args) = @_;
    my $name = _sub_name($code);
    my $reply = MT::Channel->new(1);
    $self->{jobs}->send(_encode_list($reply->id, $name, 0, @args));
    return MT::Pool::Job->_new($reply, $self);
}

# Calls $code on every item, on the workers; the results come back in the
# order of the items.  Supervised: detects dead workers while pumping jobs,
# respawns them with backoff, and either invokes on_fault or dies with the
# fault record for any in-flight job whose worker died.
sub map {
    my ($self, $code, @items) = @_;
    my $name = _sub_name($code);
    return () unless @items;

    my $n = scalar @items;
    my $res = MT::Channel->new($n < 1024 ? $n : 1024);
    my $window = int(($self->{queue_cap} || 1024) / 2);
    $window = 1 if $window < 1;
    $window = $n if $window > $n;

    my @out;
    $#out = $n - 1;
    my $sent = 0;
    my $done = 0;
    my $first_err;

    while ($done < $n) {
        # Feed jobs up to the bounded window
        while ($sent < $n && ($sent - $done) < $window) {
            $self->{jobs}->send(_encode_list($res->id, $name, $sent, $items[$sent]));
            $sent++;
        }

        # Drain available replies
        my $got = 0;
        while (1) {
            my ($ok, $msg) = $res->try_recv;
            last unless $ok;
            $got++;
            $done++;
            my ($i, $status, $value) = _decode_list($msg);
            if ($status eq 'E') {
                $first_err //= $value;
            }
            elsif ($status eq 'F') {
                if ($self->{on_fault}) {
                    $self->{on_fault}->($i, $value);
                }
                else {
                    my $m = ref($value) eq 'HASH' ? ($value->{error} // 'MT_WORKER_FAULTED') : "$value";
                    $first_err //= $m;
                }
            }
            else {
                $out[$i] = $value;
            }
        }

        if ($done < $n) {
            $self->supervise() if !$self->{client};
            if (!$got) {
                if ($self->_all_slots_exhausted()) {
                    die "MT::Pool: all workers exhausted restart limit (MT_WORKER_FAULTED)\n";
                }
                Time::HiRes::sleep(0.0005);
            }
        }
    }

    die $first_err if defined $first_err;
    return @out;
}

# Stops the workers after their queued jobs, and joins them.  Calling it
# again does nothing.
sub shutdown {
    my ($self) = @_;
    return 1 unless $self->{live};
    $self->{live} = 0;

    return 1 if $self->{client};

    if (defined $self->{name} && length $self->{name}) {
        MT::Pool::_named_unregister($self->{name});
        delete $NAMED_POOLS{$self->{name}};
    }

    my @live_wids;
    for my $slot (@{ $self->{slots} }) {
        if (defined $slot->{wid}) {
            push @live_wids, $slot->{wid};
            $self->{jobs}->send(_encode_list(0, '', 0));
        }
    }

    my @errors;
    for my $id (@live_wids) {
        eval { MT::Worker::join($id); 1 } or push @errors, $@;
    }

    if ($self->{pairs}) {
        for my $slot (@{ $self->{slots} }) {
            if ($slot->{backup_ch} && defined $slot->{backup_wid}) {
                $slot->{backup_ch}->send(_encode_list(0, '', 0));
                eval { MT::Worker::join($slot->{backup_wid}); 1 } or push @errors, $@;
            }
        }
    }

    @LIVE = grep { $_ != $self } @LIVE;
    die join('', @errors) if @errors;
    return 1;
}

sub DESTROY {
    my ($self) = @_;
    return if MT::Worker::is_worker();
    $self->shutdown if $self->{live} && !$self->{client};
}

END {
    return if MT::Worker::is_worker();
    for my $p (@LIVE) {
        next unless $p->{live} && !$p->{client};
        my $n = scalar grep { defined $_->{wid} } @{ $p->{slots} };
        warn "MT::Pool: $n worker(s) still running at exit; shutting them down\n";
        $p->shutdown;
    }
}

package MT::Pool::Job;

use strict;
use warnings;

sub _new {
    my ($class, $reply, $pool) = @_;
    return bless { reply => $reply, pool => $pool }, $class;
}

# The job's result, waiting for it if needed.  Rethrows the job's error or
# fault record; drives pool supervision while waiting so a worker crash
# never hangs join().
sub join {
    my ($self) = @_;
    return $self->{value} if $self->{done};
    my $pool = $self->{pool};
    my $msg;
    if ($pool && $pool->{live} && !$pool->{client}) {
        while (1) {
            my ($ok, $m) = $self->{reply}->try_recv;
            if ($ok) {
                $msg = $m;
                last;
            }
            $pool->supervise();
            Time::HiRes::sleep(0.0005);
        }
    }
    else {
        $msg = $self->{reply}->recv;
    }
    my (undef, $status, $value) = MT::Pool::_decode_list($msg);
    $self->{done} = 1;
    if ($status eq 'E') {
        die $value;
    }
    elsif ($status eq 'F') {
        my $m = ref($value) eq 'HASH' ? ($value->{error} // 'MT_WORKER_FAULTED') : "$value";
        die $m . "\n";
    }
    $self->{value} = $value;
    return $value;
}

1;

__END__

=head1 NAME

MT::Pool - a supervised pool of workers with named endpoints and process pairs

=head1 SYNOPSIS

    MT::Image::freeze();
    my $pool = MT::Pool->new(workers => 16, name => 'ingest');
    my $job  = $pool->run(\&work, 'payload');
    my $out  = $job->join;
    my @res  = $pool->map(\&square, 1 .. 100000);   # in order
    my @f    = $pool->faults;
    $pool->shutdown;

    # Paired pool with checkpoint takeover:
    my $paired = MT::Pool->new(workers => 4, pairs => 1);

=head1 DESCRIPTION

C<new> starts the workers, which take jobs from a channel of their own.  The
code run must be a named sub of the sealed image (C<MT::Image::freeze>, which
C<new> calls when the image is not sealed yet).

When a worker faults (e.g. C<SIGSEGV>, C<SIGBUS>, C<SIGFPE>, or unexpected
exit), the pool reaps it, records the fault (retrievable via C<< ->faults >>),
reports in-flight jobs as failed (or hands them to the designated backup worker
when C<< pairs => 1 >>), and respawns the worker with exponential backoff up to
C<max_restarts>.

=cut

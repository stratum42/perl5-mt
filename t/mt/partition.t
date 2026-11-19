#!./perl
use strict;
use warnings;
use Test::More;
use File::Temp qw(tempdir);
use File::Spec;
use Time::HiRes qw(usleep time);

BEGIN {
    chdir 't' if -d 't' && ! -d 'mt';
    unshift @INC, '../lib', 'lib', '.';
}

use MT;
use MT::Pool;
use MT::SharedImage;
use MT::Namespace;
use MT::Class;
use MT::Partition;
use MT::ProcessPair;

my $tmpdir = tempdir(CLEANUP => 1);

# Named subs for frozen-image MT::Pool tests (must be defined before freeze)
sub part_double { my ($x) = @_; return $x * 2; }
sub part_square { my ($x) = @_; return $x * $x; }

# =========================================================================
# 10a: MT::SharedImage (LPAR tier)
# =========================================================================
{
    my $img_path = File::Spec->catfile($tmpdir, 'app.mtimg');
    my $payload  = "FROZEN_IMAGE_PAYLOAD:" . ("A" x 2048);
    my $manifest = MT::SharedImage->manifest_for({
        'App::Core' => '1.00',
        'App::Math' => '2.10',
    });

    my $img = MT::SharedImage->create(
        path     => $img_path,
        manifest => $manifest,
        payload  => $payload,
    );
    ok($img && $img->id > 0, '10a: created persistent shared image');
    my $info = $img->info;
    is($info->{build_id}, MT::SharedImage->build_id, '10a: build_id matches runtime');
    ok($info->{mprotect_armed} || $info->{pkey_armed}, '10a: read-only protection armed');
    is($img->payload, $payload, '10a: creator reads back exact payload');

    # Two processes map one image and verify payload + validation
    my $pid = fork();
    defined $pid or die "fork failed: $!";
    if ($pid == 0) {
        my $child_img = MT::SharedImage->attach(
            path     => $img_path,
            manifest => $manifest,
        );
        my $ok = ($child_img->payload eq $payload) ? 0 : 1;
        $child_img->detach;
        POSIX::_exit($ok) if eval { require POSIX; 1 };
        exit($ok);
    }
    waitpid($pid, 0);
    is($? >> 8, 0, '10a: second OS process mapped shared image and verified payload');

    # Fallback path when fixed address is unavailable / forced
    my $fallback_called = 0;
    my $fb_img = MT::SharedImage->attach(
        path           => $img_path,
        manifest       => $manifest,
        force_fallback => 1,
        on_fallback    => sub { $fallback_called++ },
    );
    ok($fb_img->info->{fallback}, '10a: fallback flag set when fixed addr forced unavailable');
    is($fallback_called, 1, '10a: on_fallback callback invoked');
    is($fb_img->payload, $payload, '10a: fallback mapping still serves exact payload');
    $fb_img->detach;

    # Invalidation on module version change
    my $new_manifest = MT::SharedImage->manifest_for({
        'App::Core' => '1.01',
        'App::Math' => '2.10',
    });
    my $val_ok = MT::SharedImage->validate(path => $img_path, manifest => $manifest);
    is($val_ok->{valid}, 1, '10a: validate succeeds for matching module manifest');

    my $val_stale = MT::SharedImage->validate(path => $img_path, manifest => $new_manifest);
    is($val_stale->{valid}, 0, '10a: validate fails when module version changes');
    like($val_stale->{reason}, qr/mismatch|invalidated/i, '10a: invalidation reason reported');

    eval { MT::SharedImage->attach(path => $img_path, manifest => $new_manifest) };
    like($@, qr/invalid image/i, '10a: attach rejects invalidated image');

    $img->detach;
}

# =========================================================================
# 10b: MT::Namespace (WPAR tier)
# =========================================================================
{
    my $caps = MT::Namespace->capabilities;
    is($caps->{overlay}, 1, '10b: overlay namespaces supported');
    is(MT::Namespace->fault_model, 'shared interpreter is the kernel',
       '10b: fault model explicitly documented');
    if ($^O eq 'darwin') {
        is($caps->{clone_fs}, 0, '10b: macOS reports CLONE_FS unavailable');
        is($caps->{clone_files}, 0, '10b: macOS reports CLONE_FILES unavailable');
    }

    my $dir_a = File::Spec->catdir($tmpdir, 'group_a');
    my $dir_b = File::Spec->catdir($tmpdir, 'group_b');
    mkdir $dir_a;
    mkdir $dir_b;

    my $ns_a = MT::Namespace->new(
        name     => 'group_a',
        cwd      => $dir_a,
        umask    => 027,
        env      => { TENANT => 'ALPHA', MODE => 'strict' },
        caps     => [qw(read_fs ipc)],
        packages => {
            'Conflicting::Service' => {
                ident => sub { return 'ALPHA-v1' },
                calc  => sub { return $_[0] + 10 },
            },
        },
    );

    my $ns_b = MT::Namespace->new(
        name     => 'group_b',
        cwd      => $dir_b,
        umask    => 077,
        env      => { TENANT => 'BETA', MODE => 'relaxed' },
        caps     => [qw(read_fs write_fs net)],
        packages => {
            'Conflicting::Service' => {
                ident => sub { return 'BETA-v2' },
                calc  => sub { return $_[0] * 10 },
            },
        },
    );

    my $fd_a = $ns_a->register_fh('log', 'w', 'alpha.log');
    my $fd_b = $ns_b->register_fh('sock', 'rw', 'beta.sock');
    is($ns_a->list_fhs->[0]{path}, File::Spec->catfile($dir_a, 'alpha.log'),
       '10b: namespace A resolves fh path against its own cwd');
    is($ns_b->list_fhs->[0]{path}, File::Spec->catfile($dir_b, 'beta.sock'),
       '10b: namespace B resolves fh path against its own cwd');

    ok($ns_a->has_cap('ipc') && !$ns_a->has_cap('net'), '10b: namespace A capability set isolated');
    ok($ns_b->has_cap('net') && !$ns_b->has_cap('ipc'), '10b: namespace B capability set isolated');

    my $res_a = $ns_a->run(sub {
        my ($ns) = @_;
        return join('|',
            $ENV{TENANT},
            $ns->cwd,
            Conflicting::Service::ident(),
            Conflicting::Service::calc(5),
            MT::Namespace->current_id == $ns->id ? 'id_ok' : 'id_bad',
        );
    });
    my $res_b = $ns_b->run(sub {
        my ($ns) = @_;
        return join('|',
            $ENV{TENANT},
            $ns->cwd,
            Conflicting::Service::ident(),
            Conflicting::Service::calc(5),
            MT::Namespace->current_id == $ns->id ? 'id_ok' : 'id_bad',
        );
    });

    is($res_a, "ALPHA|$dir_a|ALPHA-v1|15|id_ok", '10b: group A ran with isolated pkg/env/cwd');
    is($res_b, "BETA|$dir_b|BETA-v2|50|id_ok",   '10b: group B ran with conflicting pkg/env/cwd');
    ok(!exists $ENV{TENANT}, '10b: host %ENV restored after namespace exit');
    is(MT::Namespace->current_id, 0, '10b: current namespace id reset to 0 after exit');

    $ns_a->destroy;
    $ns_b->destroy;
}

# =========================================================================
# 10c: MT::Class (WLM tier)
# =========================================================================
{
    my $high = MT::Class->new(
        name         => 'interactive',
        cpu          => 90,
        mem_mb       => 1,
        io_ops       => 10,
        io_window_ms => 500,
    );
    my $low = MT::Class->new(
        name         => 'batch',
        cpu          => 10,
        mem_bytes    => 4096,
        io_ops       => 3,
        io_window_ms => 500,
    );

    # Memory accounting & cap enforcement
    ok($low->charge_mem(2048), '10c: mem charge within 4096B limit succeeds');
    ok($low->charge_mem(2048), '10c: mem charge up to exact 4096B limit succeeds');
    ok(!$low->charge_mem(1),   '10c: mem charge exceeding 4096B limit denied');
    my $st = $low->stats;
    is($st->{mem_used_bytes}, 4096, '10c: mem_used_bytes tracked accurately');
    is($st->{mem_denied}, 1, '10c: mem_denied counter incremented');
    ok($low->release_mem(2048), '10c: mem release succeeds');
    ok($low->charge_mem(1024), '10c: mem charge succeeds after release');

    # I/O budget limit enforcement
    ok($low->charge_io(1), '10c: io op 1/3 allowed');
    ok($low->charge_io(2), '10c: io ops 2..3/3 allowed');
    ok(!$low->charge_io(1), '10c: io op 4/3 denied by window budget');
    is($low->stats->{io_denied}, 1, '10c: io_denied counter incremented');

    # CPU weight pacing: runaway low-weight class throttles so high-weight is not starved
    $high->bind_thread;
    $low->bind_thread;
    $high->charge_cpu(1) for 1 .. 16;
    $low->charge_cpu(1)  for 1 .. 16;
    ok($low->stats->{cpu_throttled} > 0, '10c: low-weight class throttled against higher-weight class');
    is($high->stats->{cpu_throttled}, 0, '10c: high-weight class unthrottled');

    $high->destroy;
    $low->destroy;
}

# =========================================================================
# 10d: MT::Partition (policy layer & named partitions)
# =========================================================================
{
    MT::Image::freeze() unless MT::Image::frozen();
    MT::Partition->clear_all;

    my $ns_fast  = MT::Namespace->new(name => 'ns_fast',  env => { TIER => 'fast' });
    my $ns_batch = MT::Namespace->new(name => 'ns_batch', env => { TIER => 'batch' });
    my $cls_fast  = MT::Class->new(name => 'cls_fast',  cpu => 80, mem_mb => 64);
    my $cls_batch = MT::Class->new(name => 'cls_batch', cpu => 20, mem_mb => 32);

    my $p_fast = MT::Partition->define(
        'realtime',
        namespace => $ns_fast,
        class     => $cls_fast,
        workers   => 2,
        meta      => { sla_ms => 10 },
    );
    my $p_batch = MT::Partition->define(
        'batch',
        namespace => $ns_batch,
        class     => $cls_batch,
        workers   => 2,
        meta      => { sla_ms => 5000 },
    );

    is_deeply([ MT::Partition->list ], [ 'batch', 'realtime' ], '10d: named partitions listed');
    is(MT::Partition->bind('realtime')->name, 'realtime', '10d: bind returns named partition');

    MT::Partition->add_rule(match_group => qr/^api/,  partition => 'realtime', priority => 10);
    MT::Partition->add_rule(match_job   => 'etl',     partition => 'batch',    priority => 5);
    MT::Partition->add_rule(default     => 1,         partition => 'batch',    priority => 0);

    is(MT::Partition->resolve(group => 'api-v1', job => 'query')->name, 'realtime',
       '10d: policy rule matches group regex');
    is(MT::Partition->resolve(group => 'ops', job => 'etl')->name, 'batch',
       '10d: policy rule matches job type');
    is(MT::Partition->resolve(group => 'misc', job => 'other')->name, 'batch',
       '10d: policy rule falls back to default partition');

    my $r1 = MT::Partition->dispatch(
        group => 'api-v2',
        code  => \&part_double,
        args  => [21],
    );
    is($r1, 42, '10d: dispatch executes job on resolved realtime partition pool');

    my @mapped = $p_batch->map(\&part_square, 2, 3, 4);
    is_deeply(\@mapped, [ 4, 9, 16 ], '10d: partition map executes across pool workers');

    my $st = MT::Partition->status('realtime');
    is($st->{jobs_completed}, 1, '10d: partition status tracks completed jobs');
    is($st->{class}, 'cls_fast', '10d: partition status reports bound resource class');

    MT::Partition->clear_all;
    $ns_fast->destroy;
    $ns_batch->destroy;
    $cls_fast->destroy;
    $cls_batch->destroy;
}

# =========================================================================
# 10e: MT::ProcessPair (cross-process failover + supervisor pair + TMF tx)
# =========================================================================
{
    # Part 1: Cross-process primary/backup failover over shared state + checkpoint ring
    my $pair_file = File::Spec->catfile($tmpdir, 'worker_pair.shm');
    my $img_file  = File::Spec->catfile($tmpdir, 'pair_img.mtimg');
    my $pair_init = MT::ProcessPair->create(
        path          => $pair_file,
        name          => 'orders-pair',
        image_path    => $img_file,
        hb_timeout_ms => 150,
    );
    ok($pair_init->tx_commit({ ledger_epoch => '1', balance_a => '100', balance_b => '200' }),
       '10e: multi-key atomic commit succeeds');
    my $snap = $pair_init->tx_snapshot;
    is($snap->{balance_a}, '100', '10e: tx snapshot key A visible');
    is($snap->{balance_b}, '200', '10e: tx snapshot key B visible');
    $pair_init->close;

    # Fork primary process: writes 3 checkpoints then sleeps until killed
    my $primary_pid = fork();
    defined $primary_pid or die "fork primary failed: $!";
    if ($primary_pid == 0) {
        my $p = MT::ProcessPair->attach(path => $pair_file);
        $p->register_primary;
        $p->checkpoint("state:step=1:total=10");
        $p->checkpoint("state:step=2:total=25");
        $p->checkpoint("state:step=3:total=42");
        while (1) {
            $p->heartbeat('primary');
            usleep(20_000);
        }
    }

    # Backup process (parent) waits until checkpoint 3 is visible, then kills primary
    my $backup = MT::ProcessPair->attach(path => $pair_file);
    $backup->register_backup;

    my $deadline = time() + 3.0;
    my $seen_seq = 0;
    while (time() < $deadline) {
        my $ck = $backup->last_checkpoint;
        if ($ck->{found} && $ck->{seq} >= 3) {
            $seen_seq = $ck->{seq};
            last;
        }
        usleep(10_000);
    }
    is($seen_seq, 3, '10e: backup observes primary checkpoint #3 across processes');

    kill 'KILL', $primary_pid;
    waitpid($primary_pid, 0);

    my $promoted = $backup->check_failover;
    is($promoted, 1, '10e: backup detects dead primary and atomically promotes');

    my $post_st = $backup->status;
    is($post_st->{epoch}, 2, '10e: failover incremented epoch to 2');
    is($post_st->{active_role}, MT::ProcessPair::ROLE_BACKUP, '10e: backup is now active role');
    is($post_st->{primary_pid}, $$, '10e: backup pid took over primary_pid');

    my $resumed = $backup->last_checkpoint;
    is($resumed->{data}, 'state:step=3:total=42', '10e: backup resumed from last valid checkpoint');

    my $seq4 = $backup->checkpoint("state:step=4:total=99");
    is($seq4, 4, '10e: promoted backup continues checkpoint sequence cleanly');
    $backup->close;

    # Part 2: Redundant Supervisor Pair failover
    my $sup_file = File::Spec->catfile($tmpdir, 'supervisor_pair.shm');
    my $sup_init = MT::ProcessPair->create(
        path          => $sup_file,
        name          => 'root-supervisor',
        hb_timeout_ms => 150,
        supervisor    => 1,
    );
    $sup_init->close;

    my $sup1_pid = fork();
    defined $sup1_pid or die "fork sup1 failed: $!";
    if ($sup1_pid == 0) {
        my $s1 = MT::ProcessPair->attach(path => $sup_file);
        $s1->register_primary;
        $s1->checkpoint("sup:workers_alive=4:gen=7");
        while (1) {
            $s1->heartbeat('primary');
            usleep(20_000);
        }
    }

    my $sup2 = MT::ProcessPair->attach(path => $sup_file);
    $sup2->register_backup;

    my $sup_deadline = time() + 3.0;
    while (time() < $sup_deadline) {
        my $ck = $sup2->last_checkpoint;
        last if $ck->{found} && $ck->{seq} >= 1;
        usleep(10_000);
    }

    kill 'KILL', $sup1_pid;
    waitpid($sup1_pid, 0);

    my $sup_failover = $sup2->check_failover;
    is($sup_failover, 1, '10e: backup supervisor takes over when primary supervisor is killed');
    is($sup2->status->{is_supervisor}, 1, '10e: supervisor pair flag preserved');
    is($sup2->last_checkpoint->{data}, 'sup:workers_alive=4:gen=7',
       '10e: backup supervisor recovered supervision state from checkpoint');
    $sup2->close;
}

done_testing();

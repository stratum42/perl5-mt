#!./perl
# perl5-mt: real CPAN XS modules served by XS compartments, from the
# fixtures tools/cpan-fixtures.sh installs into $PERL_MT_CPAN (default
# ~/perl5-mt-cpan).  Modules that are not installed there are skipped.
# Every case runs in a child perl: a frozen image is process-wide.

BEGIN {
    chdir 't' if -d 't';
    require './test.pl';
    set_up_inc('../lib');
}

use strict;
use warnings;
use Config;

my $cpan = $ENV{PERL_MT_CPAN} // "$ENV{HOME}/perl5-mt-cpan";
my @inc = ("$cpan/lib/perl5/$Config{archname}", "$cpan/lib/perl5");
skip_all("no CPAN fixtures under $cpan (run tools/cpan-fixtures.sh)")
    unless -d "$cpan/lib/perl5";

sub child {
    my ($code, %opt) = @_;
    my $file = tempfile();
    open my $fh, ">", $file or die "open $file: $!";
    print $fh $code;
    close $fh or die "close $file: $!";
    my $out = runperl(progfile => $file, stderr => 1,
                      switches => [ map { "-I$_" } @inc ], %opt);
    $out =~ s/\r\n/\n/g;
    return $out;
}

my %have;
for my $m (qw(Cpanel::JSON::XS DBI DBD::SQLite XML::LibXML Net::SSLeay Compress::Zlib YAML::XS)) {
    my $out = runperl(switches => [ map { "-I$_" } @inc ],
                      prog => "require $m; print qq{ok}", stderr => 1);
    $have{$m} = $out =~ /^ok/ ? 1 : 0;
    note("$m: " . ($have{$m} ? "available" : "not installed, skipped"));
}

my $prelude = <<'PRELUDE';
$| = 1;
sub say_ { print @_, "\n" }
sub run_worker { my ($sub, @args) = @_; return MT::Worker::join(MT::Worker::spawn($sub, @args)) }
sub run_workers { my ($n, $sub, @args) = @_; my @ids = map { MT::Worker::spawn($sub, $_, @args) } 1 .. $n; return map { MT::Worker::join($_) } @ids }
sub err { my ($e) = @_; $e =~ s/ at \S+ line \d+\.?\n?//; $e =~ s/\n+$//; return $e }
sub tally { my %s; $s{$_}++ for @_; return join " ", map { "$_=$s{$_}" } sort keys %s }
PRELUDE

# ---- Cpanel::JSON::XS: callbacks through the trampoline, big strings -----------

SKIP: {
    skip "Cpanel::JSON::XS not installed", 3 unless $have{'Cpanel::JSON::XS'};
    my $out = child($prelude . <<'CODE');
sub work {
    my ($id) = @_;
    require Cpanel::JSON::XS;
    my @r;
    push @r, MT::XS::is_remote(\&Cpanel::JSON::XS::encode_json) ? "remote" : "local";
    my $json = Cpanel::JSON::XS->new->canonical->allow_nonref;
    push @r, ref($json);
    my $data = { id => $id, list => [ 1, 2.5, "three", undef, \1 ], nested => { a => { b => [ { c => "d" } ] } } };
    my $ok = 0;
    for (1 .. 200) {
        my $s = $json->encode($data);
        my $d = $json->decode($s);
        $ok++ if $d->{id} == $id && $d->{nested}{a}{b}[0]{c} eq "d" && $d->{list}[2] eq "three"
                 && !defined $d->{list}[3] && $s =~ /"id":$id/;
    }
    push @r, "ok$ok";
    my $big = "x" x 1_000_000;
    my $round = $json->decode($json->encode({ big => $big }));
    push @r, length($round->{big}) == 1_000_000 ? "1MB" : "bigbad";
    eval { $json->decode("{ not json") };
    push @r, $@ =~ /malformed|expected/i ? "croaks" : "nocroak:" . err($@);
    my $seen = 0;
    my $f = Cpanel::JSON::XS->new->filter_json_object(sub { $seen++; return $_[0]{k} == 1 ? ("replaced") : () });
    my $r = $f->decode('[{"k":1},{"k":2}]');
    push @r, "filter:$seen:" . (ref $r->[1]) . ":" . $r->[0];
    push @r, $json->pretty->encode([1]) =~ /\n/ ? "chained" : "nochain";
    return join ",", @r;
}
MT::Image::freeze();
say_ tally(run_workers(16, 'main::work'));
say_ "main: ", (MT::XS::is_remote(\&Cpanel::JSON::XS::encode_json) ? "remote" : "none");
CODE
    like($out, qr/^remote,Cpanel::JSON::XS,ok200,1MB,croaks,filter:2:HASH:replaced,chained=16$/m,
         'Cpanel::JSON::XS in sixteen workers: remote stubs, 200 round trips each, a 1 MB string, croaks, filter_json_object callback, method chaining');
    like($out, qr/^main: none$/m, 'the main interpreter never loaded it');
    ok($out !~ /Attempt to free|Scalars leaked/, 'no refcount noise');
}

# ---- DBI + DBD::SQLite through MT::XS::remote ----------------------------------

SKIP: {
    skip "DBI/DBD::SQLite not installed", 2 unless $have{DBI} && $have{'DBD::SQLite'};
    my $out = child($prelude . <<'CODE');
sub work {
    my ($id) = @_;
    my @r;
    my $dbi = MT::XS::remote('DBI');
    push @r, ref($dbi);
    my $dbh = $dbi->connect("dbi:SQLite:dbname=:memory:", "", "", { RaiseError => 1, PrintError => 0 });
    push @r, ref($dbh) . ":" . $dbh->MT_class;
    $dbh->do("create table t (id integer primary key, name text)");
    my $sth = $dbh->prepare("insert into t (id, name) values (?, ?)");
    for my $i (1 .. 50) {
        $sth->bind_param(1, $i);
        $sth->bind_param(2, "name$i-$id");
        $sth->execute;
    }
    my $sel = $dbh->prepare("select id, name from t where id > ? order by id");
    $sel->execute(47);
    my @rows;
    while (my $row = $sel->fetchrow_arrayref) { push @rows, "$row->[0]:$row->[1]" }
    push @r, join("|", @rows);
    $sel->execute(49);
    my $h = $sel->fetchrow_hashref;
    push @r, "$h->{id}=$h->{name}";
    my $all = $dbh->selectall_arrayref("select count(*) from t");
    push @r, "count=$all->[0][0]";
    push @r, "autocommit=" . ($dbh->{AutoCommit} ? 1 : 0);
    push @r, "names=" . join("+", @{ $sel->{NAME} });
    eval { $dbh->do("select * from no_such_table") };
    push @r, $@ =~ /no such table/ ? "raiseerror" : "noerr:" . err($@);
    push @r, "err=" . ($dbh->errstr =~ /no such table/ ? "kept" : "lost");
    $sel->finish;
    undef $sel; undef $sth;
    $dbh->disconnect;
    my $r = join ",", @r;
    $r =~ s/-\d+\b/-N/g;                       # the worker id in the names
    return $r;
}
MT::Image::freeze();
say_ tally(run_workers(8, 'main::work'));
CODE
    like($out, qr/^MT::XS::Proxy,MT::XS::Proxy::Obj:DBI::db,48:name48-N\|49:name49-N\|50:name50-N,50=name50-N,count=50,autocommit=1,names=id\+name,raiseerror,err=kept=8$/m,
         'DBI with DBD::SQLite fully remote in eight workers: connect, do, prepare, bind_param, execute, fetchrow_arrayref/hashref, selectall_arrayref, handle attributes, RaiseError') or diag($out);
    ok($out !~ /Attempt to free|Scalars leaked/, 'no refcount noise');
}

# ---- XML::LibXML: a handle per node ---------------------------------------------------

SKIP: {
    skip "XML::LibXML not installed", 2 unless $have{'XML::LibXML'};
    my $out = child($prelude . <<'CODE');
sub work {
    my ($id) = @_;
    require XML::LibXML;
    my @r;
    push @r, MT::XS::is_remote(\&XML::LibXML::_parse_string) ? "remote" : "local";
    my $parser = XML::LibXML->new;
    my $doc = $parser->parse_string("<root id='$id'><item>one</item><item>two</item><item>three</item></root>");
    push @r, ref($doc);
    my $root = $doc->documentElement;
    push @r, $root->nodeName . ":" . ($root->getAttribute("id") == $id ? "id" : "wrongid");
    my @items = $root->findnodes('//item');
    push @r, scalar(@items) . ":" . join("+", map { $_->textContent } @items);
    push @r, MT::XS::is_handle($items[0]) ? "handles" : "copies";
    my $new = $doc->createElement("item");
    $new->appendText("four");
    $root->appendChild($new);
    push @r, scalar(my @all = $root->findnodes('//item'));
    push @r, $doc->toString =~ m{<item>four</item></root>} ? "serialised" : "noserial";
    eval { $parser->parse_string("<root><unclosed></root>") };
    push @r, $@ ? "croaks" : "nocroak";
    undef @items; undef @all; undef $root; undef $new; undef $doc;
    return join ",", @r;
}
MT::Image::freeze();
say_ tally(run_workers(8, 'main::work'));
CODE
    like($out, qr/^remote,XML::LibXML::Document,root:id,3:one\+two\+three,handles,4,serialised,croaks=8$/m,
         'XML::LibXML in eight workers: parse, documentElement, attributes, XPath, textContent, DOM mutation, serialisation, parse errors') or diag($out);
    ok($out !~ /Attempt to free|Scalars leaked/, 'no refcount noise');
}

# ---- Net::SSLeay: write-back of output buffers, stored callbacks ------------------------

SKIP: {
    skip "Net::SSLeay not installed", 2 unless $have{'Net::SSLeay'};
    my $out = child($prelude . <<'CODE');
sub work {
    my ($id) = @_;
    require Net::SSLeay;
    my @r;
    push @r, MT::XS::is_remote(\&Net::SSLeay::SSLeay) ? "remote" : "local";
    Net::SSLeay::load_error_strings();
    Net::SSLeay::SSLeay_add_ssl_algorithms();
    Net::SSLeay::randomize();
    push @r, Net::SSLeay::SSLeay() > 0 ? "version" : "noversion";
    push @r, length(Net::SSLeay::SSLeay_version(0)) > 5 ? "vstring" : "novstring";
    my $buf = "";
    my $rc = Net::SSLeay::RAND_bytes($buf, 32);
    push @r, "rand:$rc:" . length($buf);
    my $ctx = Net::SSLeay::CTX_new();
    push @r, $ctx ? "ctx" : "noctx";
    my $called = 0;
    Net::SSLeay::CTX_set_verify($ctx, &Net::SSLeay::VERIFY_PEER, sub { $called++; 1 });
    push @r, "verifycb";
    my $ssl = Net::SSLeay::new($ctx);
    push @r, $ssl ? "ssl" : "nossl";
    push @r, Net::SSLeay::get_version($ssl) =~ /TLS|SSL/ ? "tls" : "notls";
    Net::SSLeay::free($ssl);
    Net::SSLeay::CTX_free($ctx);
    push @r, "freed";
    return join ",", @r;
}
MT::Image::freeze();
say_ tally(run_workers(8, 'main::work'));
CODE
    like($out, qr/^remote,version,vstring,rand:1:32,ctx,verifycb,ssl,tls,freed=8$/m,
         'Net::SSLeay in eight workers: library init, RAND_bytes written back into the caller\'s buffer, contexts, a stored verify callback') or diag($out);
    ok($out !~ /Attempt to free|Scalars leaked/, 'no refcount noise');
}

# ---- Compress::Zlib: the shm path and streaming write-backs --------------------------

SKIP: {
    skip "Compress::Zlib not installed", 2 unless $have{'Compress::Zlib'};
    my $out = child($prelude . <<'CODE');
sub work {
    my ($id) = @_;
    require Compress::Zlib;
    require Compress::Raw::Zlib;
    my @r;
    push @r, MT::XS::is_remote(\&Compress::Raw::Zlib::crc32) ? "remote" : "local";
    my $text = join("", map { "line $_ of worker $id\n" } 1 .. 100_000);   # a few MB
    my $z = Compress::Zlib::compress($text);
    push @r, length($z) < length($text) / 5 ? "compressed" : "notcompressed";
    push @r, Compress::Zlib::uncompress($z) eq $text ? "roundtrip" : "mismatch";
    my ($d, $st) = Compress::Raw::Zlib::Deflate->new(-AppendOutput => 1);
    my $outbuf = "";
    for my $chunk (unpack "(a65536)*", $text) {
        $st = $d->deflate($chunk, $outbuf);
        return "deflate: $st" unless $st == Compress::Raw::Zlib::Z_OK();
    }
    $st = $d->flush($outbuf);
    push @r, length($outbuf) > 0 && length($outbuf) < length($text) / 5 ? "streamed" : "nostream:" . length($outbuf);
    my ($i, $ist) = Compress::Raw::Zlib::Inflate->new(-AppendOutput => 1);
    my $back = "";
    $ist = $i->inflate($outbuf, $back);
    push @r, $back eq $text ? "inflated" : "inflatebad:$ist:" . length($back);
    push @r, "crc=" . (Compress::Raw::Zlib::crc32("hello") == 907060870 ? "ok" : "bad");
    return join ",", @r;
}
MT::Image::freeze();
say_ tally(run_workers(8, 'main::work'));
CODE
    like($out, qr/^remote,compressed,roundtrip,streamed,inflated,crc=ok=8$/m,
         'Compress::Zlib in eight workers: multi-megabyte strings through the arena, streaming deflate/inflate with output buffers written back') or diag($out);
    ok($out !~ /Attempt to free|Scalars leaked/, 'no refcount noise');
}

# ---- YAML::XS: package globals set with set_remote_var -----------------------------------

SKIP: {
    skip "YAML::XS not installed", 3 unless $have{'YAML::XS'};
    my $out = child($prelude . <<'CODE');
sub work {
    my ($id) = @_;
    require YAML::XS;
    my @r;
    push @r, MT::XS::is_remote(\&YAML::XS::Dump) ? "remote" : "local";
    my $data = { id => $id, list => [ 1, "two", { three => 3 } ], text => "multi\nline" };
    my $y = YAML::XS::Dump($data);
    my $back = YAML::XS::Load($y);
    push @r, $back->{id} == $id && $back->{list}[2]{three} == 3 && $back->{text} eq "multi\nline" ? "roundtrip" : "mismatch";
    push @r, YAML::XS::Dump("123") =~ /'123'|"123"/ ? "quoted" : "bare";
    eval { YAML::XS::Load("a: [unclosed") };
    push @r, $@ =~ /YAML::XS::Load Error|parser/i ? "croaks" : "nocroak:" . err($@);
    return join ",", @r;
}
# a compartment's package variable is shared by every worker bound to it:
# one worker at a time for this part
sub globals {
    require YAML::XS;
    my @r;
    push @r, YAML::XS::Dump("123") =~ /'123'|"123"/ ? "quoted" : "bare";
    MT::XS::set_remote_var('$YAML::XS::QuoteNumericStrings', 0);
    push @r, YAML::XS::Dump("123") =~ /'123'|"123"/ ? "quoted" : "bare";
    push @r, "host=" . ($YAML::XS::QuoteNumericStrings ? 1 : 0);
    push @r, "get=" . MT::XS::get_remote_var('$YAML::XS::QuoteNumericStrings');
    MT::XS::set_remote_var('$YAML::XS::QuoteNumericStrings', 1);
    push @r, YAML::XS::Dump("123") =~ /'123'|"123"/ ? "quoted" : "bare";
    return join ",", @r;
}
MT::Image::freeze();
say_ tally(run_workers(8, 'main::work'));
say_ "globals: ", run_worker('main::globals');
CODE
    like($out, qr/^remote,roundtrip,quoted,croaks=8$/m,
         'YAML::XS in eight workers: Dump/Load, parse errors') or diag($out);
    like($out, qr/^globals: quoted,bare,host=1,get=0,quoted$/m,
         'a package global of the compartment changed with set_remote_var, the host copy untouched') or diag($out);
    ok($out !~ /Attempt to free|Scalars leaked/, 'no refcount noise');
}

# ---- every available module in sixteen workers at once, handle affinity --------------------

{
    my $mods = join ",", map { "'$_'" } grep { $have{$_} } qw(Cpanel::JSON::XS YAML::XS Compress::Zlib DBI);
    my $out = child($prelude . <<"CODE");
my \@mods = ($mods);
sub work {
    my (\$id) = \@_;
    my \@r;
    my \$comp;                       # the binding, once the first load made it
    for my \$m (\@mods) {
        \$comp //= MT::XS::stats()->{compartment} if MT::XS::stats()->{compartment} >= 0;
        if (\$m eq 'DBI') {
            next unless eval { require DBD::SQLite; 1 } || 1;
            my \$dbh = MT::XS::remote('DBI')->connect("dbi:SQLite:dbname=:memory:", "", "", { RaiseError => 1, PrintError => 0 });
            \$dbh->do("create table t (v integer)");
            \$dbh->do("insert into t values (?)", undef, \$_) for 1 .. 20;
            my \@objs = map { \$dbh->prepare("select v from t where v = ?") } 1 .. 10;
            my \$sum = 0;
            for my \$i (1 .. 200) { my \$s = \$objs[\$i % 10]; \$s->execute(\$i % 20 + 1); \$sum += \$s->fetchrow_arrayref->[0] }
            push \@r, "dbi=\$sum";
            push \@r, MT::XS::handle_compartment(\$objs[0]) == MT::XS::stats()->{compartment} ? "affine" : "elsewhere";
        }
        elsif (\$m eq 'Cpanel::JSON::XS') {
            require Cpanel::JSON::XS;
            my \$j = Cpanel::JSON::XS->new->canonical;
            my \$n = 0; \$n += \$j->decode(\$j->encode([\$_, { w => \$id }]))->[0] for 1 .. 300;
            push \@r, "json=\$n";
        }
        elsif (\$m eq 'YAML::XS') {
            require YAML::XS;
            my \$n = 0; \$n += YAML::XS::Load(YAML::XS::Dump({ v => \$_ }))->{v} for 1 .. 300;
            push \@r, "yaml=\$n";
        }
        elsif (\$m eq 'Compress::Zlib') {
            require Compress::Zlib;
            my \$n = 0; \$n += length Compress::Zlib::uncompress(Compress::Zlib::compress("z" x \$_)) for 1 .. 300;
            push \@r, "zlib=\$n";
        }
    }
    push \@r, "comp=" . (defined \$comp && MT::XS::stats()->{compartment} == \$comp ? "stable" : "moved");
    return join ",", \@r;
}
MT::Image::freeze();
say_ tally(run_workers(16, 'main::work'));
my \$bs = MT::XS::bridge_stats();
say_ "calls=", (\$bs->{remote_calls} > 0 ? "counted" : "zero");
CODE
    my $expect = join ",", map {
        $_ eq 'DBI' ? 'dbi=2100,affine' : $_ eq 'Cpanel::JSON::XS' ? 'json=45150'
        : $_ eq 'YAML::XS' ? 'yaml=45150' : 'zlib=45150'
    } grep { $have{$_} } qw(Cpanel::JSON::XS YAML::XS Compress::Zlib DBI);
    like($out, qr/^\Q$expect\E,comp=stable=16$/m,
         "sixteen workers use every available module at once (" . ($mods || 'none') . "), handles stay with their compartment")
        or diag($out);
}

# ---- a compartment dying under DBI: an error, not a hang ---------------------------------

SKIP: {
    skip "DBI/DBD::SQLite not installed", 1 unless $have{DBI} && $have{'DBD::SQLite'};
    my $out = child($prelude . <<'CODE');
use Time::HiRes qw(time);
sub work {
    my $dbh = MT::XS::remote('DBI')->connect("dbi:SQLite:dbname=:memory:", "", "", { RaiseError => 1, PrintError => 0 });
    $dbh->do("create table t (v integer)");
    MT::XS::_crash(MT::XS::handle_compartment($dbh));
    my $t = time;
    eval { $dbh->do("insert into t values (1)") };
    my $e = $@ =~ /MT_COMPARTMENT_DEAD|MT_STALE_REMOTE_HANDLE/ ? "dead" : "other:" . err($@);
    return $e . "," . (time - $t < 10 ? "fast" : "slow");
}
MT::Image::freeze();
say_ run_worker('main::work');
CODE
    like($out, qr/^dead,fast$/m, 'a compartment dying under a DBI handle surfaces as an error within seconds');
}

done_testing();

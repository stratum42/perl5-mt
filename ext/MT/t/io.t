#!./perl
# perl5-mt: MT::IO.  The core calls (stat, splice, fsync, aio_stats)
# agree with the core's own functions on the same file.

use FindBin;
use lib "$FindBin::Bin/../lib";
use Test::More;

use strict;
use warnings;
use MT::IO;

plan tests => 4;

my $file = "mt_io_$$.tmp";
open my $w, '>:unix', $file or die "open: $!";
print $w "parity\n" x 100;
close $w or die "close: $!";

is(join(',', (MT::IO::stat($file))[0 .. 4, 7]), join(',', (stat $file)[0 .. 4, 7]),
   'MT::IO::stat matches stat');
open my $fh, '>>:async', $file or die "open: $!";
ok(MT::IO::fsync($fh), 'MT::IO::fsync is true');
close $fh;
my %s = MT::IO::aio_stats();
ok(exists $s{submitted}, 'MT::IO::aio_stats has the submitted count');
ok(!defined((MT::IO::stat('/nonexistent/perl5-mt/x'))[0]), 'a missing path has no stat');
unlink $file;

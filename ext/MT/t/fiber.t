#!./perl
# perl5-mt: MT::Fiber.  The module documents the core XSUBs (see
# t/mt/fiber.t and t/mt/sched.t for their behaviour); this checks they exist.

use FindBin;
use lib "$FindBin::Bin/../lib";
use Test::More;

use strict;
use warnings;
use MT::Fiber;

plan tests => 3;

ok(defined &MT::Fiber::spawn, 'MT::Fiber::spawn is defined');
ok(defined &MT::Fiber::schedule, 'MT::Fiber::schedule is defined');
ok(defined &MT::Fiber::run, 'MT::Fiber::run is defined');

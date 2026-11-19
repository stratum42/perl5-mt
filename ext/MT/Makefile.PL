use ExtUtils::MakeMaker;

# perl5-mt: ext/MT, the Perl side of channels, cells, shared
# hashes, the worker pool, fibers and the :async IO helpers.  The XS part is
# a presence check; the primitives are in the core (mt_chan.c).

WriteMakefile(
    NAME          => 'MT',
    VERSION_FROM  => 'lib/MT.pm',
    ABSTRACT_FROM => 'lib/MT.pm',
    DEFINE        => '-DPERL_EXT',
);

sub MY::install { "install ::\n" }

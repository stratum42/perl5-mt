use ExtUtils::MakeMaker;

# perl5-mt: a legacy XS module (no PERL_XS_THREADSAFE marker, MY_CXT without
# CLONE): loaded into an XS compartment when workers are active.  Used by
# the dispatcher tests in t/mt.

WriteMakefile(
    NAME          => 'MT::XSTest::Legacy',
    VERSION_FROM  => 'lib/MT/XSTest/Legacy.pm',
    ABSTRACT_FROM => 'lib/MT/XSTest/Legacy.pm',
    DEFINE        => '-DPERL_EXT',
);

# Nothing to install.
sub MY::install { "install ::\n" }

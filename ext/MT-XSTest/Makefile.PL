use ExtUtils::MakeMaker;

# perl5-mt: a thread-safe XS module (PERL_XS_THREADSAFE) used by the
# dispatcher tests in t/mt and by ext/MT-XSTest/t.

WriteMakefile(
    NAME          => 'MT::XSTest',
    VERSION_FROM  => 'lib/MT/XSTest.pm',
    ABSTRACT_FROM => 'lib/MT/XSTest.pm',
    DEFINE        => '-DPERL_EXT -DPERL_XS_THREADSAFE=1',
);

# Nothing to install.
sub MY::install { "install ::\n" }

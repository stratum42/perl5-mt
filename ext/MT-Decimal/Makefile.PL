use ExtUtils::MakeMaker;

WriteMakefile(
    NAME          => 'MT::Decimal',
    VERSION_FROM  => 'lib/MT/Decimal.pm',
    ABSTRACT_FROM => 'lib/MT/Decimal.pm',
    DEFINE        => '-DPERL_EXT -DPERL_XS_THREADSAFE=1',
    OBJECT        => 'Decimal$(OBJ_EXT) mt_decimal$(OBJ_EXT)',
);

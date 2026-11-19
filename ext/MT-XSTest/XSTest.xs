/* perl5-mt: a thread-safe XS module.  PERL_XS_THREADSAFE is passed by
 * Makefile.PL; PERL_NO_GET_CONTEXT is required with it. */

#define PERL_NO_GET_CONTEXT
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

#define MY_CXT_KEY "MT::XSTest::_guts" XS_VERSION

typedef struct {
    IV counter;
} my_cxt_t;

START_MY_CXT

MODULE = MT::XSTest		PACKAGE = MT::XSTest

PROTOTYPES: DISABLE

BOOT:
{
    MY_CXT_INIT;
    MY_CXT.counter = 0;
}

IV
add(a, b)
    IV a
    IV b
  CODE:
    RETVAL = a + b;
  OUTPUT:
    RETVAL

IV
bump()
  PREINIT:
    dMY_CXT;
  CODE:
    RETVAL = ++MY_CXT.counter;
  OUTPUT:
    RETVAL

SV *
echo(sv)
    SV *sv
  CODE:
    RETVAL = newSVsv(sv);
  OUTPUT:
    RETVAL

IV
worker_id()
  CODE:
    RETVAL = (IV)PL_mt_worker_id;
  OUTPUT:
    RETVAL

#ifdef USE_ITHREADS

void
CLONE(...)
  CODE:
    MY_CXT_CLONE;
    MY_CXT.counter = 0;
    PERL_UNUSED_VAR(items);

#endif

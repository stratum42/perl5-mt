/* perl5-mt: a legacy XS module for the dispatcher tests.  No
 * PERL_XS_THREADSAFE, so no marker: while workers are active it is loaded
 * into an XS compartment and every function below is reached through a
 * host stub.  It uses MY_CXT without CLONE on purpose (the freeze refuses
 * such a module in the image), keeps process-global counters, calls back
 * into Perl, croaks, modifies its arguments in place, attaches magic and
 * installs a compiler hook when asked. */

#define PERL_NO_GET_CONTEXT
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

#define MY_CXT_KEY "MT::XSTest::Legacy::_guts" XS_VERSION

typedef struct {
    IV counter;
} my_cxt_t;

START_MY_CXT

static IV S_global_counter = 0;
static IV S_objects_alive = 0;

static int
S_hooky_keyword_plugin(pTHX_ char *keyword_ptr, STRLEN keyword_len, OP **op_ptr)
{
    return Perl_keyword_plugin_standard(aTHX_ keyword_ptr, keyword_len, op_ptr);
}

MODULE = MT::XSTest::Legacy		PACKAGE = MT::XSTest::Legacy

PROTOTYPES: DISABLE

BOOT:
{
    MY_CXT_INIT;
    MY_CXT.counter = 0;
    if (PerlEnv_getenv("MT_XSTEST_HOOK")) {
        /* a module that installs a compiler hook cannot be served by a
         * compartment */
        PL_keyword_plugin = S_hooky_keyword_plugin;
    }
}

void
echo(...)
  PREINIT:
    I32 i;
  PPCODE:
    EXTEND(SP, items);
    for (i = 0; i < items; i++)
        PUSHs(sv_2mortal(newSVsv(ST(i))));

IV
sum(...)
  PREINIT:
    I32 i;
  CODE:
    RETVAL = 0;
    for (i = 0; i < items; i++)
        RETVAL += SvIV(ST(i));
  OUTPUT:
    RETVAL

SV *
big_string(n)
    IV n
  CODE:
    RETVAL = newSV(n + 1);
    SvPOK_only(RETVAL);
    memset(SvPVX(RETVAL), 'x', n);
    SvPVX(RETVAL)[n] = '\0';
    SvCUR_set(RETVAL, n);
  OUTPUT:
    RETVAL

IV
length_of(sv)
    SV *sv
  CODE:
    RETVAL = (IV)sv_len(sv);
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

IV
global_bump()
  CODE:
    RETVAL = ++S_global_counter;
  OUTPUT:
    RETVAL

IV
pid()
  CODE:
    RETVAL = (IV)getpid();
  OUTPUT:
    RETVAL

void
callback(cb, ...)
    SV *cb
  PREINIT:
    I32 i, count;
  PPCODE:
    {
        PUSHMARK(SP);
        EXTEND(SP, items - 1);
        for (i = 1; i < items; i++)
            PUSHs(ST(i));
        PUTBACK;
        count = call_sv(cb, G_LIST);
        SPAGAIN;
        /* the results sit above our arguments: move them down */
        for (i = 0; i < count; i++)
            ST(i) = SP[i - count + 1];
        XSRETURN(count);
    }

SV *
callback_scalar(cb, arg)
    SV *cb
    SV *arg
  PREINIT:
    I32 count;
  CODE:
    {
        dSP;
        PUSHMARK(SP);
        XPUSHs(arg);
        PUTBACK;
        count = call_sv(cb, G_SCALAR);
        SPAGAIN;
        RETVAL = count ? newSVsv(POPs) : newSV_type(SVt_NULL);
        PUTBACK;
    }
  OUTPUT:
    RETVAL

SV *
callback_catch(cb)
    SV *cb
  CODE:
    {
        dSP;
        PUSHMARK(SP);
        PUTBACK;
        call_sv(cb, G_VOID | G_DISCARD | G_EVAL);
        SPAGAIN;
        RETVAL = SvTRUE(ERRSV) ? newSVsv(ERRSV) : newSVpvs("no error");
        PUTBACK;
    }
  OUTPUT:
    RETVAL

void
croak_with(msg)
    SV *msg
  CODE:
    Perl_croak_sv(aTHX_ msg);

void
die_object(klass)
    const char *klass
  CODE:
    {
        HV * const hv = newHV();
        (void)hv_stores(hv, "code", newSViv(42));
        Perl_croak_sv(aTHX_ sv_bless(newRV_noinc((SV *)hv), gv_stashpv(klass, GV_ADD)));
    }

void
modify_arg(sv, newval)
    SV *sv
    SV *newval
  CODE:
    sv_setsv(sv, newval);

void
modify_ref(ref, newval)
    SV *ref
    SV *newval
  CODE:
    if (!SvROK(ref))
        croak("modify_ref needs a reference");
    sv_setsv(SvRV(ref), newval);

SV *
new_object(klass, ...)
    const char *klass
  PREINIT:
    I32 i;
  CODE:
    {
        HV * const hv = newHV();
        for (i = 1; i + 1 < items; i += 2)
            (void)hv_store_ent(hv, ST(i), newSVsv(ST(i + 1)), 0);
        S_objects_alive++;
        RETVAL = sv_bless(newRV_noinc((SV *)hv), gv_stashpv(klass, GV_ADD));
    }
  OUTPUT:
    RETVAL

SV *
new_array_object(klass, ...)
    const char *klass
  PREINIT:
    I32 i;
  CODE:
    {
        AV * const av = newAV();
        for (i = 1; i < items; i++)
            av_push(av, newSVsv(ST(i)));
        S_objects_alive++;
        RETVAL = sv_bless(newRV_noinc((SV *)av), gv_stashpv(klass, GV_ADD));
    }
  OUTPUT:
    RETVAL

SV *
obj_get(obj, key)
    SV *obj
    SV *key
  CODE:
    {
        HE *he;
        if (!SvROK(obj) || SvTYPE(SvRV(obj)) != SVt_PVHV)
            croak("obj_get: not a hash object");
        he = hv_fetch_ent((HV *)SvRV(obj), key, 0, 0);
        RETVAL = he ? newSVsv(HeVAL(he)) : newSV_type(SVt_NULL);
    }
  OUTPUT:
    RETVAL

const char *
obj_class(obj)
    SV *obj
  CODE:
    RETVAL = SvROK(obj) && SvOBJECT(SvRV(obj)) ? HvNAME(SvSTASH(SvRV(obj))) : "";
  OUTPUT:
    RETVAL

IV
objects_alive()
  CODE:
    RETVAL = S_objects_alive;
  OUTPUT:
    RETVAL

IV
same_object(a, b)
    SV *a
    SV *b
  CODE:
    RETVAL = SvROK(a) && SvROK(b) && SvRV(a) == SvRV(b);
  OUTPUT:
    RETVAL

void
magic_on_arg(sv)
    SV *sv
  CODE:
    sv_magicext(sv, NULL, PERL_MAGIC_ext, NULL, NULL, 0);

SV *
pkg_var()
  CODE:
    {
        SV * const sv = get_sv("MT::XSTest::Legacy::setting", GV_ADD);
        RETVAL = newSVsv(sv);
    }
  OUTPUT:
    RETVAL

SV *
code_ref()
  CODE:
    RETVAL = newRV_inc((SV *)get_cv("MT::XSTest::Legacy::sum", 0));
  OUTPUT:
    RETVAL

SV *
call_perl_side(arg)
    SV *arg
  PREINIT:
    I32 count;
  CODE:
    {
        dSP;
        PUSHMARK(SP);
        XPUSHs(arg);
        PUTBACK;
        count = call_pv("MT::XSTest::Legacy::perl_side", G_SCALAR);
        SPAGAIN;
        RETVAL = count ? newSVsv(POPs) : newSV_type(SVt_NULL);
        PUTBACK;
    }
  OUTPUT:
    RETVAL

void
sleep_ms(ms)
    IV ms
  CODE:
    {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }

# a blocking call that holds the compartment (and so the worker's
# remote call) for ms milliseconds; the fibers of the host park meanwhile
void
nap_ms(ms)
    IV ms
  CODE:
    {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }

void
exit_now(code)
    IV code
  CODE:
    my_exit((U32)code);

MODULE = MT::XSTest::Legacy		PACKAGE = MT::XSTest::Legacy::Obj

void
DESTROY(obj)
    SV *obj
  CODE:
    PERL_UNUSED_VAR(obj);
    S_objects_alive--;

/*    mt_fiber.c
 *
 *    perl5-mt: fibers.  A fiber is a Perl call stack on its own C
 *    stack (a guarded stacklet from mmap) that is entered with resume and
 *    left with yield: asymmetric coroutines, a fiber returns control to the
 *    context that resumed it and nothing else.
 *
 *    the scheduler on top of resume.  A scheduled fiber is resumed
 *    only by the loop (Perl_mt_sched_run), which runs on the context that
 *    called run, so a scheduled fiber hands control back by yielding to the
 *    loop.  The tick hooks PL_signalhook and switches fibers at a
 *    PERL_ASYNC_CHECK; see MT_README.md.
 *
 *    The C stack is switched by mt_ctx_swap (mt_ctx.h).  The Perl state is
 *    switched by S_mt_vm_save and S_mt_vm_load, which copy exactly the fields
 *    of struct mt_vmstate (mt_fiber.h) and nothing else.  Every context saves
 *    its own state before it leaves, and the one that leaves loads the state
 *    of the context it switches to before the switch, so the code after a
 *    swap never has to load anything.
 *
 *    The fiber records and the MT::Fiber XSUBs live here too.  Ids are
 *    worker id << 32 | a per-interpreter sequence, so an id from another
 *    worker is never found.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#include "EXTERN.h"
#define PERL_IN_MT_FIBER_C
#include "perl.h"
#include "XSUB.h"
#include "mt_fiber.h"
#include <sys/mman.h>
#include <unistd.h>

/* The usable size of a stacklet, and the least MT_FIBER_STACK may ask for
 * (bytes).  A guard page of the system page size sits below every stacklet
 * (16 KiB on Apple Silicon). */
#define MT_FIBER_STACK_DEFAULT (64 * 1024)
#define MT_FIBER_STACK_MIN     (16 * 1024)

/* aTHX->I<name> is the interpreter field PL_<name> stands for; PL_na and
 * friends are not visible to the core itself. */
#ifdef MT_FIBER_ASAN
/* ASan keeps track of the stack it runs on, and a switch made by mt_ctx_swap
 * has to be announced to it: the start before the swap, with the stack that
 * is switched to, and the finish in the context that the swap brings back.
 * A context that finishes for good passes no fake-stack slot to the start. */
void __sanitizer_start_switch_fiber(void **fake_stack_save,
                                    const void *bottom, size_t size);
void __sanitizer_finish_switch_fiber(void *fake_stack_save,
                                     const void **bottom_old, size_t *size_old);
#  define MT_ASAN_START(fake, to) \
    __sanitizer_start_switch_fiber((fake), (to)->asan_bottom, (to)->asan_size)
#  define MT_ASAN_START_FINAL(to) \
    __sanitizer_start_switch_fiber(NULL, (to)->asan_bottom, (to)->asan_size)
#  define MT_ASAN_FINISH(fake) __sanitizer_finish_switch_fiber((fake), NULL, NULL)
#else
#  define MT_ASAN_START(fake, to) ((void)0)
#  define MT_ASAN_START_FINAL(to) ((void)0)
#  define MT_ASAN_FINISH(fake) ((void)0)
#endif

#define MT_VM_SAVE(f) vm->f = aTHX->I##f;
#define MT_VM_LOAD(f) aTHX->I##f = vm->f;

/* ---- the interpreter-state swap ------------------------------------------ */

/* Unwind CvDEPTH for every live CXt_SUB on this fiber's context stack (from
 * innermost to outermost) and ensure each frame's pad is marked PadBUSY so
 * no other fiber can reuse that pad slot while this fiber is suspended. */
static void
S_mt_fiber_save_cvdepth(pTHX_ const struct mt_vmstate *vm)
{
    PERL_SI *si;
    for (si = vm->curstackinfo; si; si = si->si_prev) {
        I32 i;
        for (i = si->si_cxix; i >= 0; i--) {
            PERL_CONTEXT * const cx = &si->si_cxstack[i];
            if (CxTYPE(cx) == CXt_SUB && cx->blk_sub.cv && cx->blk_sub.paddepth > 0) {
                CV * const cv = cx->blk_sub.cv;
                PADLIST * const pl = CvPADLIST(cv);
                if (pl && cx->blk_sub.paddepth <= PadlistMAX(pl)) {
                    PAD * const p = PadlistARRAY(pl)[cx->blk_sub.paddepth];
                    if (p)
                        PadBUSY_on(p);
                }
                CvDEPTH(cv) = cx->blk_sub.olddepth;
            }
        }
    }
}

static void
S_mt_fiber_load_cvdepth_si(pTHX_ PERL_SI *si)
{
    I32 i;
    if (si->si_prev)
        S_mt_fiber_load_cvdepth_si(aTHX_ si->si_prev);
    for (i = 0; i <= si->si_cxix; i++) {
        PERL_CONTEXT * const cx = &si->si_cxstack[i];
        if (CxTYPE(cx) == CXt_SUB && cx->blk_sub.cv && cx->blk_sub.paddepth > 0) {
            CV * const cv = cx->blk_sub.cv;
            cx->blk_sub.olddepth = CvDEPTH(cv);
            CvDEPTH(cv) = cx->blk_sub.paddepth;
        }
    }
}

static void
S_mt_vm_save(pTHX_ struct mt_vmstate *vm)
{
    MT_VMSTATE_FIELDS(MT_VM_SAVE)
    vm->rx_slab = aTHX->Iregmatch_slab;
    vm->rx_state = aTHX->Iregmatch_state;
    vm->defgv_av = (PL_defgv && GvGP(PL_defgv)) ? GvAV(PL_defgv) : NULL;
#ifdef DEBUGGING
    vm->scopestack_name = aTHX->Iscopestack_name;
#endif
    S_mt_fiber_save_cvdepth(aTHX_ vm);
}

static void
S_mt_vm_load(pTHX_ const struct mt_vmstate *vm)
{
    MT_VMSTATE_FIELDS(MT_VM_LOAD)
    aTHX->Iregmatch_slab = vm->rx_slab;
    aTHX->Iregmatch_state = vm->rx_state;
    if (PL_defgv && GvGP(PL_defgv))
        GvAV(PL_defgv) = vm->defgv_av;
#ifdef DEBUGGING
    aTHX->Iscopestack_name = vm->scopestack_name;
#endif
    if (vm->curstackinfo)
        S_mt_fiber_load_cvdepth_si(aTHX_ vm->curstackinfo);
}

/* The Perl stacks of a fresh fiber: one stackinfo with an argument stack,
 * and mark, scope, save and tmps stacks, all of them small and growing on
 * demand.  The shape is that of Perl_init_stacks (perl.c). */
static void
S_mt_fiber_new_pstacks(pTHX_ struct mt_vmstate *vm)
{
    PERL_SI *si = new_stackinfo_flags(MT_FIBER_STACK_ITEMS,
                                      MT_FIBER_CX_ITEMS, 0);
    si->si_type = PERLSI_MAIN;
#ifdef PERL_USE_HWM
    si->si_stack_hwm = 0;
#endif
#ifdef DEBUGGING
    si->si_markoff = 0;
#endif
    vm->curstackinfo = si;
    vm->curstack = si->si_stack;
    vm->mainstack = si->si_stack;
    vm->stack_base = AvARRAY(si->si_stack);
    vm->stack_sp = vm->stack_base;
    vm->stack_max = vm->stack_base + AvMAX(si->si_stack);

    Newxz(vm->tmps_stack, MT_FIBER_TMPS_ITEMS, SV *);
    vm->tmps_ix = -1;
    vm->tmps_floor = -1;
    vm->tmps_max = MT_FIBER_TMPS_ITEMS;

    Newxz(vm->markstack, MT_FIBER_MARK_ITEMS, Stack_off_t);
    vm->markstack_ptr = vm->markstack;
    vm->markstack_max = vm->markstack + MT_FIBER_MARK_ITEMS;

    Newxz(vm->scopestack, MT_FIBER_SCOPE_ITEMS, I32);
    vm->scopestack_ix = 0;
    vm->scopestack_max = MT_FIBER_SCOPE_ITEMS;
#ifdef DEBUGGING
    Newxz(vm->scopestack_name, MT_FIBER_SCOPE_ITEMS, const char *);
#endif

    /* as init_stacks: the save stack keeps SS_MAXPUSH slots in reserve */
    Newxz(vm->savestack, MT_FIBER_SAVE_ITEMS + SS_MAXPUSH, ANY);
    vm->savestack_ix = 0;
    vm->savestack_max = MT_FIBER_SAVE_ITEMS;
}

/* Frees the Perl stacks of a fiber that has finished (or whose state is
 * not live any more).  The argument stacks are not real AVs, so their
 * elements are not released here. */
static void
S_mt_fiber_free_pstacks(pTHX_ struct mt_vmstate *vm)
{
    PERL_SI *si = vm->curstackinfo;
    PERL_SI *prev;
    regmatch_slab *slab;

    if (!si)
        return;
    while (si->si_next)
        si = si->si_next;
    for (; si; si = prev) {
        prev = si->si_prev;
        SvREFCNT_dec(MUTABLE_SV(si->si_stack));
        Safefree(si->si_cxstack);
        Safefree(si);
    }
    Safefree(vm->markstack);
    Safefree(vm->scopestack);
#ifdef DEBUGGING
    Safefree(vm->scopestack_name);
#endif
    Safefree(vm->savestack);
    Safefree(vm->tmps_stack);
    /* regexec allocates the slabs of the match state lazily, as a list
     * that is walked through prev to the first one */
    slab = vm->rx_slab;
    if (slab) {
        while (slab->prev)
            slab = slab->prev;
        while (slab) {
            regmatch_slab *next = slab->next;
            Safefree(slab);
            slab = next;
        }
    }
    if (vm->defgv_av)
        SvREFCNT_dec(MUTABLE_SV(vm->defgv_av));
    Zero(vm, 1, struct mt_vmstate);
}

/* ---- stacklets ------------------------------------------------------------ */

static size_t
S_mt_fiber_stack_len(size_t pg)
{
    size_t len = MT_FIBER_STACK_DEFAULT;
    const char *env = getenv("MT_FIBER_STACK");
    if (env && *env) {
        char *end;
        unsigned long v = strtoul(env, &end, 10);
        if (*end == '\0' && v >= MT_FIBER_STACK_MIN)
            len = (size_t)v;
    }
    return (len + pg - 1) / pg * pg;
}

/* Maps a stacklet for f: a guard page, then the usable stack. */
static void
S_mt_fiber_map(pTHX_ struct mt_fiber *f)
{
    const size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    const size_t len = S_mt_fiber_stack_len(pg);
    const size_t maplen = len + pg;
    void *p = mmap(NULL, maplen, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        croak_mt(MT_CTX_STACK);
    if (mprotect(p, pg, PROT_NONE) != 0) {
        munmap(p, maplen);
        croak_mt(MT_CTX_STACK);
    }
    f->stack_map = p;
    f->stack_map_len = maplen;
    f->stack_base = (char *)p + pg;
    f->stack_len = len;
    PL_mt_fiber_mmaps++;
    if (++PL_mt_fiber_nmapped > PL_mt_fiber_peak)
        PL_mt_fiber_peak = PL_mt_fiber_nmapped;
}

static void
S_mt_fiber_unmap(pTHX_ struct mt_fiber *f)
{
    if (!f->stack_map)
        return;
    munmap(f->stack_map, f->stack_map_len);
    f->stack_map = NULL;
    f->stack_base = NULL;
    f->stack_map_len = 0;
    f->stack_len = 0;
    PL_mt_fiber_nmapped--;
}

/* ---- records -------------------------------------------------------------- */

static UV
S_mt_fiber_next_id(pTHX)
{
    return ((UV)PL_mt_worker_id << (UVSIZE * 4)) | PL_mt_fiber_next_id++;
}

/* A record for a new fiber.  A parked record is reused, and keeps its
 * stacklet; every other field starts out zero. */
static struct mt_fiber *
S_mt_fiber_new(pTHX)
{
    struct mt_fiber *f;
    if (PL_mt_fiber_free_stacks) {
        void *map, *base;
        size_t maplen, len;
        f = PL_mt_fiber_free_stacks;
        PL_mt_fiber_free_stacks = f->next_free;
        if (f->stack_map)
            PL_mt_fiber_nfree--;
        map = f->stack_map;
        base = f->stack_base;
        maplen = f->stack_map_len;
        len = f->stack_len;
        Zero(f, 1, struct mt_fiber);
        f->stack_map = map;
        f->stack_base = base;
        f->stack_map_len = maplen;
        f->stack_len = len;
    }
    else
        Newxz(f, 1, struct mt_fiber);
    f->id = S_mt_fiber_next_id(aTHX);
    return f;
}

static void
S_mt_fiber_register(pTHX_ struct mt_fiber *f)
{
    if (!PL_mt_fibers)
        PL_mt_fibers = newHV();
    (void)hv_store(PL_mt_fibers, (char *)&f->id, sizeof f->id,
                   newSViv(PTR2IV(f)), 0);
}

static struct mt_fiber *
S_mt_fiber_lookup(pTHX_ UV id)
{
    SV **svp = PL_mt_fibers
        ? hv_fetch(PL_mt_fibers, (char *)&id, sizeof id, 0) : NULL;
    if (!svp)
        croak_mt(MT_NOT_A_FIBER);
    return INT2PTR(struct mt_fiber *, SvIVX(*svp));
}

/* The record of the context that is running now: the current fiber, or
 * the record of the original stack. */
static struct mt_fiber *
S_mt_fiber_self(pTHX)
{
    if (PL_mt_fiber_current)
        return PL_mt_fiber_current;
    if (!PL_mt_fiber_main)
        Newxz(PL_mt_fiber_main, 1, struct mt_fiber);
    return PL_mt_fiber_main;
}

static void
S_mt_fiber_check(pTHX_ const struct mt_fiber *f)
{
    if (f->flags & MT_FIBER_DONE)
        croak_mt(MT_FIBER_FINISHED);
    if (f->flags & MT_FIBER_RUNNING)
        croak_mt(MT_FIBER_BUSY);
    if (PL_parser)
        croak_mt(MT_YIELD_IN_COMPILE);
}

/* Takes f out of the interpreter's table and parks its record, with the
 * stacklet, on the free list.  The record is reused by the next spawn. */
static void
S_mt_fiber_park(pTHX_ struct mt_fiber *f)
{
    if (PL_mt_fibers)
        (void)hv_delete(PL_mt_fibers, (char *)&f->id, sizeof f->id, G_DISCARD);
    SvREFCNT_dec(f->entry_cv);
    f->entry_cv = NULL;
    SvREFCNT_dec(MUTABLE_SV(f->args));
    f->args = NULL;
    SvREFCNT_dec(f->result);
    f->result = NULL;
    SvREFCNT_dec(f->error);
    f->error = NULL;
    if (f->flags & MT_FIBER_STARTED)
        S_mt_fiber_free_pstacks(aTHX_ &f->vm);
    if (f->stack_map)
        PL_mt_fiber_nfree++;
    f->next_free = PL_mt_fiber_free_stacks;
    PL_mt_fiber_free_stacks = f;
}

/* ---- running a fiber ----------------------------------------------------- */

/* Runs the fiber's code with its arguments, as a G_EVAL call: a die leaves
 * the message in error, and a normal return leaves the value in result. */
static void
S_mt_fiber_run(pTHX_ struct mt_fiber *f)
{
    dSP;
    SSize_t i, n = 0;
    I32 count;

    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    if (f->args) {
        n = AvFILL(f->args) + 1;
        EXTEND(SP, n);
        for (i = 0; i < n; i++) {
            SV **svp = av_fetch(f->args, i, 0);
            PUSHs(svp ? *svp : &PL_sv_undef);
        }
        /* the arguments are on the stack now: the array lives until the
         * fiber's tmps are freed below */
        sv_2mortal(MUTABLE_SV(f->args));
        f->args = NULL;
    }
    PUTBACK;
    count = call_sv(f->entry_cv, G_SCALAR | G_EVAL);
    SPAGAIN;
    if (SvTRUE(ERRSV)) {
        f->error = newSVsv(ERRSV);
        f->flags |= MT_FIBER_DIED;
    }
    else
        f->result = count ? newSVsv(POPs) : newSV(0);
    PUTBACK;
    FREETMPS;
    LEAVE;
}

/* The first code on a fresh stacklet.  It is entered by the first
 * mt_ctx_swap into the fiber, and it never returns: the last thing it does
 * is switch to the resumer, and nothing switches back. */
/* The bottom of every fiber's jump chain.  JMPENV_JUMP longjmps only when
 * the innermost environment has a predecessor; without one, exit() would end
 * the process from inside the fiber.  Nothing ever jumps to this one. */
static JMPENV S_mt_fiber_base_env;

static void
S_mt_fiber_entry(void *arg)
{
    dTHX;
    struct mt_fiber *const f = (struct mt_fiber *)arg;
    struct mt_fiber *to;
    int ret;
    dJMPENV;

#ifdef MT_FIBER_ASAN
    /* the first run: the stack we came from is the resumer's, and ASan
     * learns its bounds here (the main stack's, for the main record) */
    __sanitizer_finish_switch_fiber(f->asan_fake,
                                    &f->resumer->asan_bottom,
                                    &f->resumer->asan_size);
#endif

    /* the fiber's root: a die is caught by call_sv, but exit() unwinds to
     * here (JMPENV_PUSH also re-arms the boot image's key, which the signal
     * return of a longjmp on x86 may have reset) */
    JMPENV_PUSH(ret);
    if (ret == 0)
        S_mt_fiber_run(aTHX_ f);
    else if (ret == 2) {
        SV *msg = Perl_mess(aTHX_ "perl5-mt: %s (%s)",
                            Perl_mt_err_msg(MT_EXIT_IN_FIBER),
                            Perl_mt_err_name(MT_EXIT_IN_FIBER));
        f->error = newSVsv(msg);
        f->flags |= MT_FIBER_DIED;
        FREETMPS;
    }
    else {
        f->error = newSVsv(ERRSV);
        f->flags |= MT_FIBER_DIED;
        FREETMPS;
    }
    JMPENV_POP;

    f->flags |= MT_FIBER_DONE;
    to = f->resumer;
    S_mt_vm_save(aTHX_ &f->vm);
    S_mt_vm_load(aTHX_ &to->vm);
    f->flags &= ~MT_FIBER_RUNNING;
    PL_mt_fiber_current = (to == PL_mt_fiber_main) ? NULL : to;
    MT_ASAN_START_FINAL(to);
    mt_ctx_swap(&f->ctx, &to->ctx);
    /* not reached: a finished fiber is never resumed */
    abort();
}

/* The first resume of f: its state is a copy of the resumer's, with fresh
 * stacks and the transient state (the jump environment, the match state,
 * the current op and pad) cleared. */
static void
S_mt_fiber_start(pTHX_ struct mt_fiber *f, const struct mt_vmstate *from)
{
    f->vm = *from;
    S_mt_fiber_new_pstacks(aTHX_ &f->vm);
    f->vm.top_env = &S_mt_fiber_base_env;
    f->vm.restartop = NULL;
    f->vm.restartjmpenv = NULL;
    f->vm.in_eval = 0;
    f->vm.localizing = 0;
    f->vm.delaymagic = 0;
    f->vm.curpm = NULL;
    f->vm.curpm_under = NULL;
    f->vm.reg_curpm = NULL;
    f->vm.rx_slab = NULL;
    f->vm.rx_state = NULL;
    f->vm.multideref_pc = NULL;
    f->vm.sortcop = NULL;
    f->vm.sortstash = NULL;
    f->vm.op = NULL;
    f->vm.comppad = NULL;
    f->vm.curpad = NULL;
    f->vm.defgv_av = NULL;
    f->vm.curcop = &PL_compiling;
    S_mt_vm_load(aTHX_ &f->vm);
    mt_ctx_make(&f->ctx, f->stack_base, f->stack_len, S_mt_fiber_entry, f);
#ifdef MT_FIBER_ASAN
    f->asan_bottom = f->stack_base;
    f->asan_size = f->stack_len;
#endif
    f->flags |= MT_FIBER_STARTED;
}

/* ---- the API ------------------------------------------------------------- */

void
Perl_mt_fiber_resume(pTHX_ struct mt_fiber *f)
{
    struct mt_fiber *from;
    PERL_ARGS_ASSERT_MT_FIBER_RESUME;

    S_mt_fiber_check(aTHX_ f);
    /* everything that can fail happens before the state changes */
    if (!(f->flags & MT_FIBER_STARTED) && !f->stack_map)
        S_mt_fiber_map(aTHX_ f);
    from = S_mt_fiber_self(aTHX);
    S_mt_vm_save(aTHX_ &from->vm);
    f->resumer = from;
    f->flags |= MT_FIBER_RUNNING;
    PL_mt_fiber_current = f;
    if (f->flags & MT_FIBER_STARTED)
        S_mt_vm_load(aTHX_ &f->vm);
    else
        S_mt_fiber_start(aTHX_ f, &from->vm);
    MT_ASAN_START(&from->asan_fake, f);
    mt_ctx_swap(&from->ctx, &f->ctx);
    MT_ASAN_FINISH(from->asan_fake);
    /* back: f yielded or finished, and loaded our state before it left */
}

void
Perl_mt_fiber_yield(pTHX)
{
    struct mt_fiber *const f = PL_mt_fiber_current;
    struct mt_fiber *to;

    if (!f)
        croak_mt(MT_NOT_IN_FIBER);
    if (PL_parser)
        croak_mt(MT_YIELD_IN_COMPILE);
    to = f->resumer;
    S_mt_vm_save(aTHX_ &f->vm);
    S_mt_vm_load(aTHX_ &to->vm);
    f->flags &= ~MT_FIBER_RUNNING;
    PL_mt_fiber_current = (to == PL_mt_fiber_main) ? NULL : to;
    MT_ASAN_START(&f->asan_fake, to);
    mt_ctx_swap(&f->ctx, &to->ctx);
    MT_ASAN_FINISH(f->asan_fake);
    /* back: the resume that woke us stored its arguments in f->args */
}

/* ---- the scheduler -------------------------------------------------------
 *
 * The loop runs on the context that called run: the main context, or the
 * fiber that called run.  A scheduled fiber is resumed by the loop only, so
 * when it returns control it is either finished (the loop releases it),
 * parked (a wake puts it back on the queue), or ready (it passed, or made a
 * plain yield, and the loop puts it back on the queue).  A fiber on the
 * queue is never running: ready refuses a running fiber, and resume refuses
 * a scheduled one. */

/* Appends f to the tail of the ready queue.  The callers have checked f. */
static void
S_mt_sched_enqueue(pTHX_ struct mt_fiber *f)
{
    if (f->flags & MT_FIBER_READY)
        return;
    f->flags |= MT_FIBER_READY;
    f->next_ready = NULL;
    if (PL_mt_sched_tail)
        PL_mt_sched_tail->next_ready = f;
    else
        PL_mt_sched_head = f;
    PL_mt_sched_tail = f;
    PL_mt_sched_nready++;
}

/* Takes the first fiber off the ready queue, which must not be empty. */
static struct mt_fiber *
S_mt_sched_dequeue(pTHX)
{
    struct mt_fiber *const f = PL_mt_sched_head;
    PL_mt_sched_head = f->next_ready;
    if (!PL_mt_sched_head)
        PL_mt_sched_tail = NULL;
    f->next_ready = NULL;
    f->flags &= ~MT_FIBER_READY;
    PL_mt_sched_nready--;
    return f;
}

void
Perl_mt_sched_ready(pTHX_ struct mt_fiber *f)
{
    PERL_ARGS_ASSERT_MT_SCHED_READY;
    if (f->flags & MT_FIBER_DONE)
        croak_mt(MT_FIBER_FINISHED);
    if (f->flags & MT_FIBER_RUNNING)
        croak_mt(MT_FIBER_BUSY);
    if (f->flags & MT_FIBER_PARKED) {
        f->flags &= ~MT_FIBER_PARKED;
        PL_mt_sched_nparked--;
    }
    f->flags |= MT_FIBER_SCHEDULED;
    S_mt_sched_enqueue(aTHX_ f);
}

/* A wake is a ready, and it asks the next async check for a tick: the tick
 * is what lets a woken fiber run while the waker computes.  Without
 * preemption the wake waits for the fiber that woke it to pass or park. */
void
Perl_mt_sched_wake(pTHX_ struct mt_fiber *f)
{
    PERL_ARGS_ASSERT_MT_SCHED_WAKE;
    Perl_mt_sched_ready(aTHX_ f);
    PL_mt_sched_pending = 1;
    if (PL_mt_sched_preempt)
        PL_sig_pending = 1;
}

/* The scheduled fiber that is running now gives up the processor until
 * the loop resumes it, parked: a wake puts it back on the queue. */
void
Perl_mt_sched_park(pTHX)
{
    struct mt_fiber *const f = PL_mt_fiber_current;
    if (!f)
        croak_mt(MT_NOT_IN_FIBER);
    if (!(f->flags & MT_FIBER_SCHEDULED))
        croak_mt(MT_FIBER_NOT_SCHEDULED);
    if (PL_parser)
        croak_mt(MT_YIELD_IN_COMPILE);
    f->flags |= MT_FIBER_PARKED;
    PL_mt_sched_nparked++;
    Perl_mt_fiber_yield(aTHX);
}

/* The scheduled fiber that is running now goes to the tail of the queue
 * and yields to the loop.  It is ready when the loop comes back to it. */
void
Perl_mt_sched_pass(pTHX)
{
    struct mt_fiber *const f = PL_mt_fiber_current;
    if (!f)
        croak_mt(MT_NOT_IN_FIBER);
    if (!(f->flags & MT_FIBER_SCHEDULED))
        croak_mt(MT_FIBER_NOT_SCHEDULED);
    if (PL_parser)
        croak_mt(MT_YIELD_IN_COMPILE);
    S_mt_sched_enqueue(aTHX_ f);
    Perl_mt_fiber_yield(aTHX);
}

/* the scheduler's hooks, shared by the XS waiters (mt_xs_compartment.c)
 * and the aio waiters (perlio_async.c).  Arm them when the first waiter parks. */
void
Perl_mt_sched_arm(pTHX)
{
    PL_mt_sched_idle_hook = Perl_mt_sched_idle;
    PL_mt_sched_poll_hook = Perl_mt_sched_poll;
}

/* The poll of the scheduler, once a round: never blocks.  It serves the
 * XS replies and the aio completions that arrived while the fibers ran. */
void
Perl_mt_sched_poll(pTHX)
{
    int woke = 0;
    Perl_mt_xs_drain(aTHX_ &woke);
    Perl_mt_aio_drain_completions(aTHX_ &woke);
    Perl_mt_chan_drain(aTHX_ &woke);
    if (!PL_mt_xs_waiters && !PL_mt_aio_waiters && !PL_mt_chan_waiters)
        PL_mt_sched_poll_hook = NULL;
}

/* The idle hook of the scheduler: no fiber is ready, so the loop waits here
 * for a parked call to finish: an XS reply on this worker's reply vector, or
 * an aio completion in its ring.  Returns once a fiber was woken, or when
 * nothing is parked.  Both sources are checked before any wait.
 * A wait that could miss one source is bounded: an aio completion does not
 * ring the vector, so while both sources are pending the vector wait is
 * one millisecond at most (MT_AIO_BOUND_NS); the ring wait alone is bounded
 * the same way so that a vector post is not missed for long. */
#define MT_AIO_BOUND_NS  1000000ull

void
Perl_mt_sched_idle(pTHX)
{
    for (;;) {
        int woke = 0;
        U32 doorbell = 0;
        int xs;
        Perl_mt_xs_drain(aTHX_ &woke);
        Perl_mt_aio_drain_completions(aTHX_ &woke);
        Perl_mt_chan_drain(aTHX_ &woke);
        if (woke)
            return;
        if (!PL_mt_xs_waiters && !PL_mt_aio_waiters && !PL_mt_chan_waiters)
            return;
        xs = Perl_mt_xs_bell(aTHX_ &doorbell);
        if (PL_mt_aio_pending && xs) {
            /* both sources: poll the ring and the doorbell for the policy's
             * budget, then park on the vector for a bounded time */
            const uint64_t t0 = perl_krt_now_ns();
            while (!perl_krt_aio_ready(PL_mt_aio)
                   && perl_krt_now_ns() - t0 < PL_mt_xs_poll.cur_ns) {
                if (Perl_mt_xs_bell_changed(aTHX_ doorbell))
                    break;
                perl_krt_cpu_relax();
            }
            if (!perl_krt_aio_ready(PL_mt_aio))
                Perl_mt_xs_bell_wait(aTHX_ doorbell, MT_AIO_BOUND_NS);
        }
        else if (PL_mt_aio_pending) {
            (void)perl_krt_aio_wait(PL_mt_aio, MT_AIO_BOUND_NS);
        }
        else if (xs) {
            /* a parked channel waiter cannot be woken through the vector:
             * the wait is bounded then */
            Perl_mt_xs_bell_wait(aTHX_ doorbell, PL_mt_chan_waiters
                                 ? MT_AIO_BOUND_NS : MT_XS_WAIT_BACKSTOP_NS);
        }
        else if (PL_mt_chan_waiters) {
            Perl_mt_chan_idle_wait(aTHX);
        }
        else
            return;
    }
}

/* A finished scheduled fiber leaves the table: its result (or its error)
 * goes to the results hash, to be collected by join, and its record and
 * stacklet go to the free list.  The record is reused by the next spawn. */
static void
S_mt_sched_release(pTHX_ struct mt_fiber *f)
{
    AV *const av = newAV();
    if (f->flags & MT_FIBER_DIED) {
        av_push(av, newSViv(1));
        av_push(av, f->error ? f->error : newSV(0));
        f->error = NULL;
    }
    else {
        av_push(av, newSViv(0));
        av_push(av, f->result ? f->result : newSV(0));
        f->result = NULL;
    }
    if (!PL_mt_sched_results)
        PL_mt_sched_results = newHV();
    (void)hv_store(PL_mt_sched_results, (char *)&f->id, sizeof f->id,
                   newRV_noinc(MUTABLE_SV(av)), 0);
    S_mt_fiber_park(aTHX_ f);
}

/* Runs the scheduled fibers until the queue is empty.  The loop resumes the
 * head, and when it comes back the fiber is finished, parked, or ready.  An
 * empty queue calls the idle hook; if that made nothing ready, parked fibers
 * left over mean a deadlock.  Returns the fibers that finished. */
U32
Perl_mt_sched_run(pTHX)
{
    struct mt_fiber *runner;
    U32 nfinished = 0;

    if (PL_mt_sched_fiber)
        croak_mt(MT_SCHED_NESTED);
    ENTER;
    /* a croak out of the loop leaves the loop; the state is restored */
    runner = S_mt_fiber_self(aTHX);
    SAVEVPTR(PL_mt_sched_fiber);
    PL_mt_sched_fiber = runner;
    for (;;) {
        /* a poll never blocks: it serves what arrived while fibers ran */
        if (PL_mt_sched_poll_hook)
            PL_mt_sched_poll_hook(aTHX);
        if (PL_mt_sched_head) {
            struct mt_fiber *f = PL_mt_sched_head;
            /* everything that can fail is checked before the head leaves
             * the queue: a fiber lost there would never run again */
            if (PL_parser)
                croak_mt(MT_YIELD_IN_COMPILE);
            if (!(f->flags & MT_FIBER_STARTED) && !f->stack_map)
                S_mt_fiber_map(aTHX_ f);
            if (PL_mt_sched_pending && PL_mt_sched_preempt)
                PL_sig_pending = 1;     /* the tick of the fiber we start */
            (void)S_mt_sched_dequeue(aTHX);
            Perl_mt_fiber_resume(aTHX_ f);
            if (f->flags & MT_FIBER_DONE) {
                S_mt_sched_release(aTHX_ f);
                nfinished++;
            }
            else {
                /* a plain yield left its values; they go nowhere */
                if (f->result) {
                    SvREFCNT_dec(f->result);
                    f->result = NULL;
                }
                if (!(f->flags & (MT_FIBER_READY | MT_FIBER_PARKED)))
                    S_mt_sched_enqueue(aTHX_ f);
            }
            continue;
        }
        if (PL_mt_sched_idle_hook) {
            PL_mt_sched_idle_hook(aTHX);
            if (PL_mt_sched_head)
                continue;
        }
        if (PL_mt_sched_nparked)
            Perl_croak(aTHX_ "perl5-mt: scheduler deadlock: %d parked, "
                       "none ready (MT_FIBER_DEADLOCK)", (int)PL_mt_sched_nparked);
        break;
    }
    PL_mt_sched_nfinished = nfinished;
    LEAVE;
    return nfinished;
}

/* The signal hook that chains the previous one (Perl_despatch_signals, or
 * a module's hook).  Real signals go to the previous hook, which dispatches
 * them; a veto leaves PL_sig_pending set, as it always did.  Then the tick:
 * a wake asked for one, and the current fiber can be switched only when it
 * is scheduled and is not the loop's own context, and no compilation is
 * under way.  A refused tick keeps the wake pending; the loop arms the next
 * check before it starts a fiber. */
static int
S_mt_sched_real_signal(pTHX)
{
    int sig;
    /* the table is made when the first handler is set (gv.c) */
    if (!PL_psig_pend)
        return 0;
    for (sig = 1; sig < SIG_SIZE; sig++) {
        if (PL_psig_pend[sig])
            return 1;
    }
    return 0;
}

static void
S_mt_sched_signalhook(pTHX)
{
    struct mt_fiber *const f = PL_mt_fiber_current;
    /* cleared first, so that a signal that arrives during the test is
     * seen by the next check and not lost */
    PL_sig_pending = 0;
    if (S_mt_sched_real_signal(aTHX)) {
        PL_sig_pending = 1;
        PL_mt_sched_prev_signalhook(aTHX);
    }
    if (PL_mt_sched_pending && PL_mt_sched_preempt && f
        && (f->flags & MT_FIBER_SCHEDULED) && f != PL_mt_sched_fiber
        && !PL_parser)
    {
        PL_mt_sched_pending = 0;
        PL_mt_sched_ticks++;
        Perl_mt_sched_pass(aTHX);
    }
}

/* Per-interpreter state at the start of a worker: nothing crosses into it
 * from the image, which may have run fibers before the freeze.  The state
 * that a worker inherits on purpose is not reset here: the signal hook
 * chain, the idle hook and the preemption switch. */
void
Perl_mt_fiber_init(pTHX)
{
    /* the :async ring is this interpreter's own: a clone must
     * not drain its source's ring */
    Perl_mt_aio_reset(aTHX);
    PL_mt_chan_waiters = NULL;
    PL_mt_fiber_current = NULL;
    PL_mt_fiber_main = NULL;
    PL_mt_fibers = NULL;
    PL_mt_fiber_free_stacks = NULL;
    PL_mt_fiber_next_id = 1;
    PL_mt_fiber_nfree = 0;
    PL_mt_fiber_nmapped = 0;
    PL_mt_fiber_peak = 0;
    PL_mt_fiber_mmaps = 0;
    PL_mt_sched_head = NULL;
    PL_mt_sched_tail = NULL;
    PL_mt_sched_nready = 0;
    PL_mt_sched_nparked = 0;
    PL_mt_sched_pending = 0;
    PL_mt_sched_fiber = NULL;
    PL_mt_sched_results = NULL;
    PL_mt_sched_nfinished = 0;
    PL_mt_sched_ticks = 0;
}

static void
S_mt_fiber_drop(pTHX_ struct mt_fiber *f)
{
    SvREFCNT_dec(f->entry_cv);
    SvREFCNT_dec(MUTABLE_SV(f->args));
    SvREFCNT_dec(f->result);
    SvREFCNT_dec(f->error);
    S_mt_fiber_unmap(aTHX_ f);
    Safefree(f);
}

/* Interpreter destruction: every record and stacklet goes.  A fiber that
 * never finished is not unwound, so its Perl stacks are left to the arena
 * sweep. */
void
Perl_mt_fiber_destroy(pTHX)
{
    struct mt_fiber *f;

    Perl_mt_aio_destroy(aTHX);
    if (PL_mt_fibers) {
        HE *he;
        hv_iterinit(PL_mt_fibers);
        while ((he = hv_iternext(PL_mt_fibers))) {
            f = INT2PTR(struct mt_fiber *, SvIVX(HeVAL(he)));
            if ((f->flags & MT_FIBER_STARTED) && (f->flags & MT_FIBER_DONE))
                S_mt_fiber_free_pstacks(aTHX_ &f->vm);
            S_mt_fiber_drop(aTHX_ f);
        }
        SvREFCNT_dec(MUTABLE_SV(PL_mt_fibers));
        PL_mt_fibers = NULL;
    }
    while ((f = PL_mt_fiber_free_stacks)) {
        PL_mt_fiber_free_stacks = f->next_free;
        S_mt_fiber_drop(aTHX_ f);
    }
    PL_mt_fiber_nfree = 0;
    if (PL_mt_fiber_main) {
        Safefree(PL_mt_fiber_main);
        PL_mt_fiber_main = NULL;
    }
    PL_mt_fiber_current = NULL;
    /* the queue points into the records freed above */
    PL_mt_sched_head = NULL;
    PL_mt_sched_tail = NULL;
    PL_mt_sched_nready = 0;
    PL_mt_sched_nparked = 0;
    PL_mt_sched_fiber = NULL;
    if (PL_mt_sched_results) {
        SvREFCNT_dec(MUTABLE_SV(PL_mt_sched_results));
        PL_mt_sched_results = NULL;
    }
}

/* ---- the MT::Fiber XSUBs ------------------------------------------------- */

/* Stores the arguments of a resume: appended to the spawn arguments before
 * the first start, and a fresh list after it. */
static void
S_mt_fiber_set_args(pTHX_ struct mt_fiber *f, SSize_t n, SV **argv)
{
    SSize_t i;
    if (!f->args)
        f->args = newAV();
    for (i = 0; i < n; i++)
        av_push(f->args, newSVsv(argv[i]));
}

/* The values a yield or a finish left for the resumer, as mortal copies. */
static AV *
S_mt_fiber_take(pTHX_ struct mt_fiber *f)
{
    AV *out = MUTABLE_AV(sv_2mortal(MUTABLE_SV(newAV())));
    if (f->flags & MT_FIBER_DONE) {
        if (f->result)
            av_push(out, newSVsv(f->result));
    }
    else if (f->result) {
        AV *y = MUTABLE_AV(SvRV(f->result));
        SSize_t i, n = AvFILL(y) + 1;
        for (i = 0; i < n; i++) {
            SV **svp = av_fetch(y, i, 0);
            av_push(out, newSVsv(svp ? *svp : &PL_sv_undef));
        }
        SvREFCNT_dec(f->result);
        f->result = NULL;
    }
    return out;
}

/* The arguments that the resume which woke a yield passed in. */
static AV *
S_mt_fiber_take_args(pTHX_ struct mt_fiber *f)
{
    AV *out = MUTABLE_AV(sv_2mortal(MUTABLE_SV(newAV())));
    if (f->args) {
        SSize_t i, n = AvFILL(f->args) + 1;
        for (i = 0; i < n; i++) {
            SV **svp = av_fetch(f->args, i, 0);
            av_push(out, newSVsv(svp ? *svp : &PL_sv_undef));
        }
        SvREFCNT_dec(MUTABLE_SV(f->args));
        f->args = NULL;
    }
    return out;
}

/* Leaves vals as the XSUB's return values, replacing its arguments: all of
 * them in list context, the last one (undef if none) in scalar context. */
static void
S_mt_fiber_ret(pTHX_ I32 ax, AV *vals)
{
    SV **sp = PL_stack_base + ax - 1;
    SSize_t i, n = AvFILL(vals) + 1;
    if (GIMME_V == G_SCALAR) {
        SV **svp = n ? av_fetch(vals, n - 1, 0) : NULL;
        EXTEND(sp, 1);
        *++sp = svp ? sv_2mortal(newSVsv(*svp)) : &PL_sv_undef;
    }
    else {
        EXTEND(sp, n);
        for (i = 0; i < n; i++) {
            SV **svp = av_fetch(vals, i, 0);
            *++sp = sv_2mortal(newSVsv(svp ? *svp : &PL_sv_undef));
        }
    }
    PL_stack_sp = sp;
}

/* The record of a new fiber that runs code with the arguments argv[1..n-1]
 * (argv[0] is the code reference). */
static struct mt_fiber *
S_mt_fiber_spawn(pTHX_ SSize_t items, SV **argv)
{
    struct mt_fiber *f;
    AV *args;
    SSize_t i;

    if (!SvROK(argv[0]) || SvTYPE(SvRV(argv[0])) != SVt_PVCV)
        Perl_croak(aTHX_ "MT::Fiber::spawn: the first argument is not a code reference");
    f = S_mt_fiber_new(aTHX);
    f->entry_cv = SvREFCNT_inc(SvRV(argv[0]));
    args = newAV();
    av_extend(args, items);
    for (i = 1; i < items; i++)
        av_push(args, newSVsv(argv[i]));
    f->args = args;
    S_mt_fiber_register(aTHX_ f);
    return f;
}

XS(XS_MT_Fiber_spawn);
XS(XS_MT_Fiber_spawn)
{
    dXSARGS;
    struct mt_fiber *f;

    if (items < 1)
        croak_xs_usage(cv, "code, ...");
    f = S_mt_fiber_spawn(aTHX_ items, &ST(0));
    ST(0) = sv_2mortal(newSVuv(f->id));
    XSRETURN(1);
}

XS(XS_MT_Fiber_resume);
XS(XS_MT_Fiber_resume)
{
    dXSARGS;
    struct mt_fiber *f;

    if (items < 1)
        croak_xs_usage(cv, "id, ...");
    f = S_mt_fiber_lookup(aTHX_ SvUV(ST(0)));
    if (f->flags & MT_FIBER_SCHEDULED)
        croak_mt(MT_FIBER_IS_SCHEDULED);
    S_mt_fiber_check(aTHX_ f);
    S_mt_fiber_set_args(aTHX_ f, items - 1, &ST(1));
    Perl_mt_fiber_resume(aTHX_ f);
    S_mt_fiber_ret(aTHX_ ax, S_mt_fiber_take(aTHX_ f));
    return;
}

XS(XS_MT_Fiber_yield);
XS(XS_MT_Fiber_yield)
{
    dXSARGS;
    struct mt_fiber *const f = PL_mt_fiber_current;
    AV *out;
    SSize_t i;

    if (!f)
        croak_mt(MT_NOT_IN_FIBER);
    if (PL_parser)
        croak_mt(MT_YIELD_IN_COMPILE);
    out = newAV();
    av_extend(out, items);
    for (i = 0; i < items; i++)
        av_push(out, newSVsv(ST(i)));
    f->result = newRV_noinc(MUTABLE_SV(out));
    Perl_mt_fiber_yield(aTHX);
    S_mt_fiber_ret(aTHX_ ax, S_mt_fiber_take_args(aTHX_ f));
    return;
}

/* The scheduler's result of a finished fiber that nobody has joined yet, as
 * the mortal RV to [died, value], or NULL.  Taking it removes it. */
static SV *
S_mt_sched_take(pTHX_ UV id)
{
    if (!PL_mt_sched_results)
        return NULL;
    return hv_delete(PL_mt_sched_results, (char *)&id, sizeof id, 0);
}

XS(XS_MT_Fiber_join);
XS(XS_MT_Fiber_join)
{
    dXSARGS;
    struct mt_fiber *f;
    SV *ret, *ent;
    UV id;

    if (items != 1)
        croak_xs_usage(cv, "id");
    id = SvUV(ST(0));
    if ((ent = S_mt_sched_take(aTHX_ id))) {
        AV *const av = MUTABLE_AV(SvRV(ent));
        SV **const died = av_fetch(av, 0, 0);
        SV **const val = av_fetch(av, 1, 0);
        if (SvIV(*died))
            croak_sv(sv_mortalcopy(*val));
        ST(0) = sv_mortalcopy(*val);
        XSRETURN(1);
    }
    f = S_mt_fiber_lookup(aTHX_ id);
    if (f->flags & MT_FIBER_SCHEDULED)
        croak_mt(MT_FIBER_NOT_FINISHED);
    while (!(f->flags & MT_FIBER_DONE)) {
        S_mt_fiber_check(aTHX_ f);
        S_mt_fiber_set_args(aTHX_ f, 0, NULL);
        Perl_mt_fiber_resume(aTHX_ f);
        (void)S_mt_fiber_take(aTHX_ f);   /* the yields are discarded */
    }
    if (f->flags & MT_FIBER_DIED) {
        SV *err = sv_mortalcopy(f->error);
        S_mt_fiber_park(aTHX_ f);
        croak_sv(err);
    }
    ret = f->result ? sv_mortalcopy(f->result) : sv_newmortal();
    S_mt_fiber_park(aTHX_ f);
    ST(0) = ret;
    XSRETURN(1);
}

XS(XS_MT_Fiber_current);
XS(XS_MT_Fiber_current)
{
    dXSARGS;
    PERL_UNUSED_VAR(items);
    ST(0) = sv_2mortal(newSVuv(PL_mt_fiber_current ? PL_mt_fiber_current->id : 0));
    XSRETURN(1);
}

XS(XS_MT_Fiber_is_finished);
XS(XS_MT_Fiber_is_finished)
{
    dXSARGS;
    struct mt_fiber *f;
    UV id;
    if (items != 1)
        croak_xs_usage(cv, "id");
    id = SvUV(ST(0));
    if (PL_mt_sched_results && hv_exists(PL_mt_sched_results, (char *)&id, sizeof id)) {
        ST(0) = &PL_sv_yes;
        XSRETURN(1);
    }
    f = S_mt_fiber_lookup(aTHX_ id);
    ST(0) = (f->flags & MT_FIBER_DONE) ? &PL_sv_yes : &PL_sv_no;
    XSRETURN(1);
}

XS(XS_MT_Fiber_count);
XS(XS_MT_Fiber_count)
{
    dXSARGS;
    PERL_UNUSED_VAR(items);
    ST(0) = sv_2mortal(newSVuv(PL_mt_fibers ? HvUSEDKEYS(PL_mt_fibers) : 0));
    XSRETURN(1);
}

XS(XS_MT_Fiber_stats);
XS(XS_MT_Fiber_stats)
{
    dXSARGS;
    HV *hv = newHV();
    PERL_UNUSED_VAR(items);
    (void)hv_stores(hv, "mapped", newSVuv(PL_mt_fiber_nmapped));
    (void)hv_stores(hv, "free", newSVuv(PL_mt_fiber_nfree));
    (void)hv_stores(hv, "peak", newSVuv(PL_mt_fiber_peak));
    (void)hv_stores(hv, "mmaps", newSVuv(PL_mt_fiber_mmaps));
    ST(0) = sv_2mortal(newRV_noinc(MUTABLE_SV(hv)));
    XSRETURN(1);
}

/* ---- the scheduler XSUBs -------------------------------------------------- */

XS(XS_MT_Fiber_schedule);
XS(XS_MT_Fiber_schedule)
{
    dXSARGS;
    struct mt_fiber *f;

    if (items < 1)
        croak_xs_usage(cv, "code, ...");
    f = S_mt_fiber_spawn(aTHX_ items, &ST(0));
    Perl_mt_sched_ready(aTHX_ f);
    ST(0) = sv_2mortal(newSVuv(f->id));
    XSRETURN(1);
}

XS(XS_MT_Fiber_ready);
XS(XS_MT_Fiber_ready)
{
    dXSARGS;
    if (items != 1)
        croak_xs_usage(cv, "id");
    Perl_mt_sched_ready(aTHX_ S_mt_fiber_lookup(aTHX_ SvUV(ST(0))));
    XSRETURN_EMPTY;
}

XS(XS_MT_Fiber_wake);
XS(XS_MT_Fiber_wake)
{
    dXSARGS;
    if (items != 1)
        croak_xs_usage(cv, "id");
    Perl_mt_sched_wake(aTHX_ S_mt_fiber_lookup(aTHX_ SvUV(ST(0))));
    XSRETURN_EMPTY;
}

XS(XS_MT_Fiber_park);
XS(XS_MT_Fiber_park)
{
    dXSARGS;
    PERL_UNUSED_VAR(items);
    Perl_mt_sched_park(aTHX);
    XSRETURN_EMPTY;
}

XS(XS_MT_Fiber_pass);
XS(XS_MT_Fiber_pass)
{
    dXSARGS;
    PERL_UNUSED_VAR(items);
    Perl_mt_sched_pass(aTHX);
    XSRETURN_EMPTY;
}

XS(XS_MT_Fiber_run);
XS(XS_MT_Fiber_run)
{
    dXSARGS;
    PERL_UNUSED_VAR(items);
    ST(0) = sv_2mortal(newSVuv(Perl_mt_sched_run(aTHX)));
    XSRETURN(1);
}

/* Sets the preemption switch when given a value; returns the old one. */
XS(XS_MT_Fiber_preempt);
XS(XS_MT_Fiber_preempt)
{
    dXSARGS;
    const int old = PL_mt_sched_preempt;
    if (items > 1)
        croak_xs_usage(cv, "[bool]");
    if (items == 1)
        PL_mt_sched_preempt = SvTRUE(ST(0)) ? 1 : 0;
    ST(0) = old ? &PL_sv_yes : &PL_sv_no;
    XSRETURN(1);
}

XS(XS_MT_Fiber_sched_stats);
XS(XS_MT_Fiber_sched_stats)
{
    dXSARGS;
    HV *hv = newHV();
    PERL_UNUSED_VAR(items);
    (void)hv_stores(hv, "ready", newSVuv(PL_mt_sched_nready));
    (void)hv_stores(hv, "parked", newSVuv(PL_mt_sched_nparked));
    (void)hv_stores(hv, "finished", newSVuv(PL_mt_sched_nfinished));
    (void)hv_stores(hv, "ticks", newSVuv(PL_mt_sched_ticks));
    ST(0) = sv_2mortal(newRV_noinc(MUTABLE_SV(hv)));
    XSRETURN(1);
}

void
Perl_mt_boot_fiber(pTHX)
{
    /* the tick: chained once, in front of whatever hook is installed */
    if (PL_signalhook != S_mt_sched_signalhook) {
        PL_mt_sched_prev_signalhook = PL_signalhook;
        PL_signalhook = S_mt_sched_signalhook;
    }
    /* MT_FIBER_PREEMPT=1 switches preemption on for this program; the
     * workers inherit it with the rest of the interpreter */
    {
        const char *env = getenv("MT_FIBER_PREEMPT");
        if (env && *env)
            PL_mt_sched_preempt = (*env == '1');
    }
    newXS_flags("MT::Fiber::schedule", XS_MT_Fiber_schedule, __FILE__, "$@", 0);
    newXS_flags("MT::Fiber::ready", XS_MT_Fiber_ready, __FILE__, "$", 0);
    newXS_flags("MT::Fiber::wake", XS_MT_Fiber_wake, __FILE__, "$", 0);
    newXS_flags("MT::Fiber::park", XS_MT_Fiber_park, __FILE__, "", 0);
    newXS_flags("MT::Fiber::pass", XS_MT_Fiber_pass, __FILE__, "", 0);
    newXS_flags("MT::Fiber::run", XS_MT_Fiber_run, __FILE__, "", 0);
    newXS_flags("MT::Fiber::preempt", XS_MT_Fiber_preempt, __FILE__, ";$", 0);
    newXS_flags("MT::Fiber::sched_stats", XS_MT_Fiber_sched_stats, __FILE__, "", 0);
    newXS_flags("MT::Fiber::spawn", XS_MT_Fiber_spawn, __FILE__, "$@", 0);
    newXS_flags("MT::Fiber::resume", XS_MT_Fiber_resume, __FILE__, "$@", 0);
    newXS_flags("MT::Fiber::yield", XS_MT_Fiber_yield, __FILE__, "@", 0);
    newXS_flags("MT::Fiber::join", XS_MT_Fiber_join, __FILE__, "$", 0);
    newXS_flags("MT::Fiber::current", XS_MT_Fiber_current, __FILE__, "", 0);
    newXS_flags("MT::Fiber::is_finished", XS_MT_Fiber_is_finished, __FILE__, "$", 0);
    newXS_flags("MT::Fiber::count", XS_MT_Fiber_count, __FILE__, "", 0);
    newXS_flags("MT::Fiber::stats", XS_MT_Fiber_stats, __FILE__, "", 0);
}

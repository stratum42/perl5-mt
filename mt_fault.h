/*    mt_fault.h
 *
 *    perl5-mt: fail-fast policy, fault event model, and
 *    supervisor liveness heartbeats over the krt vector table.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_MT_FAULT_H
#define PERL_MT_FAULT_H

#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include "mt_err.h"
#include "krt/krt.h"

/* ---- fail-fast policy & fault event model --------------------- */

/* Policy classification:
 *   RECOVERABLE: the offending worker or compartment stops at once; the
 *                host process survives, records the fault, and notifies
 *                observers so supervision can recover.
 *   FATAL:       host-level invariant violation or corruption; the process
 *                terminates immediately (fail-fast). */
#define MT_FAULT_RECOVERABLE   1u
#define MT_FAULT_FATAL         2u

/* Unit kinds */
#define MT_UNIT_MAIN           0u
#define MT_UNIT_WORKER         1u
#define MT_UNIT_COMPARTMENT    2u

/* Fault cause codes */
enum perl_mt_fault_cause {
    MT_FAULT_NONE = 0,
    MT_FAULT_ERR_CROAK = 1,
    MT_FAULT_HEARTBEAT_TIMEOUT = 2,
    MT_FAULT_SIGNAL = 3,
    MT_FAULT_COMPARTMENT_CRASH = 4,
    MT_FAULT_WORKER_EXIT = 5,
    MT_FAULT_CAP_EXHAUSTED = 6,
    MT_FAULT_INVARIANT = 7,
    MT_FAULT_OOM = 8,
    MT_FAULT_TEST_RECOVERABLE = 9,
    MT_FAULT_TEST_FATAL = 10
};

#define MT_FAULT_MSG_MAX 256

typedef struct perl_mt_fault {
    uint64_t seq;                    /* monotonic sequence (0 = no fault)  */
    uint64_t timestamp_ns;           /* monotonic clock (perl_krt_now_ns)  */
    uint32_t cause;                  /* enum perl_mt_fault_cause           */
    uint32_t severity;               /* MT_FAULT_RECOVERABLE / _FATAL      */
    uint32_t unit_kind;              /* MT_UNIT_*                          */
    uint32_t unit_id;                /* worker or compartment id (0: main) */
    uint32_t generation;             /* unit generation / epoch            */
    int32_t  err_code;               /* enum perl_mt_err (MT_OK if n/a)    */
    char     message[MT_FAULT_MSG_MAX];
} perl_mt_fault_t;

typedef struct perl_mt_fault_site {
    const char *file;
    const char *function;
    const char *err_name;
    uint32_t    severity;            /* MT_FAULT_RECOVERABLE / _FATAL      */
    const char *rationale;
} perl_mt_fault_site_t;

/* ---- supervisor heartbeats over krt_vec.h --------------------- */

#define MT_SUP_MAX_UNITS       256u  /* 4 PBA words of 64 units each       */
#define MT_SUP_DEFAULT_HB_MS   100u  /* PERL_MT_HEARTBEAT_MS default       */
#define MT_SUP_DEFAULT_MISS    3u    /* PERL_MT_HEARTBEAT_MISS default     */

#define MT_SUP_STATE_FREE      0u
#define MT_SUP_STATE_LIVE      1u
#define MT_SUP_STATE_FAULTED   2u
#define MT_SUP_STATE_EXITED    3u

typedef struct perl_mt_sup_unit_info {
    uint32_t slot;
    uint32_t unit_kind;
    uint32_t unit_id;
    uint32_t generation;
    uint32_t state;
    uint32_t missed_beats;
    uint64_t last_beat_ns;
    uint64_t age_ms;
    uint64_t beat_count;
} perl_mt_sup_unit_info_t;

/* ---- C API -------------------------------------------------------------- */

void     Perl_mt_fault_init(void);
uint64_t Perl_mt_fault_record(uint32_t cause, uint32_t severity,
                              uint32_t unit_kind, uint32_t unit_id,
                              uint32_t generation, int32_t err_code,
                              const char *msg);
int      Perl_mt_fault_last(perl_mt_fault_t *out);
uint64_t Perl_mt_fault_count(void);
void     Perl_mt_fault_reset(void);
int      Perl_mt_fault_drain_events(uint64_t *after_seq,
                                    perl_mt_fault_t *buf, int max_events);

const char *Perl_mt_fault_cause_name(uint32_t cause);
const char *Perl_mt_fault_severity_name(uint32_t severity);
const char *Perl_mt_fault_unit_kind_name(uint32_t unit_kind);
const perl_mt_fault_site_t *Perl_mt_fault_sites(size_t *count);

#if defined(PERL_CORE) || defined(PERL_EXT)
void     Perl_mt_fault_raise(pTHX_ uint32_t cause, uint32_t severity,
                             uint32_t unit_kind, uint32_t unit_id,
                             uint32_t generation, int32_t err_code,
                             const char *msg);
U32      Perl_mt_sup_tick(pTHX);
SV      *Perl_mt_fault_last_sv(pTHX);
AV      *Perl_mt_fault_drain_av(pTHX_ uint64_t *after_seq);
AV      *Perl_mt_fault_sites_av(pTHX);
AV      *Perl_mt_sup_status_av(pTHX);
SV      *Perl_mt_pool_encode_list(pTHX_ SV **items, I32 count);
AV      *Perl_mt_pool_decode_list(pTHX_ SV *blob);
int      Perl_mt_aio_test_combined_sleep(pTHX);
#endif

void     Perl_mt_sup_init(void);
int32_t  Perl_mt_sup_register(uint32_t unit_kind, uint32_t unit_id,
                              uint32_t generation);
void     Perl_mt_sup_unregister(int32_t slot);
void     Perl_mt_sup_beat_slot(int32_t slot);
void     Perl_mt_sup_maybe_beat(void);
void     Perl_mt_sup_set_tls_slot(int32_t slot);
int32_t  Perl_mt_sup_get_tls_slot(void);
void     Perl_mt_sup_pause_heartbeat(int32_t slot, int paused);
int32_t  Perl_mt_sup_find_slot(uint32_t unit_kind, uint32_t unit_id);
size_t   Perl_mt_sup_snapshot(perl_mt_sup_unit_info_t *out, size_t max_units);
uint64_t Perl_mt_sup_bench_beat(int32_t slot, uint64_t iters);
void     Perl_mt_sup_config(uint32_t *hb_ms, uint32_t *hb_miss);

/* combined aio + doorbell wait (krt_linux.c / krt_darwin.c) */
int      perl_krt_aio_wait_doorbell(perl_krt_aio_t *ctx,
                                    volatile uint32_t *doorbell,
                                    uint32_t expected,
                                    uint32_t wait_flags,
                                    uint64_t timeout_ns);

/* ---- per-worker signal containment ---------------------------- */

typedef struct perl_mt_worker_sig_ctx {
    sigjmp_buf             jmp;
    volatile sig_atomic_t  armed;
    volatile sig_atomic_t  caught_sig;
    volatile uintptr_t     fault_addr;
    volatile int           fault_code;
    uint32_t               worker_id;
    void                  *altstack_mem;
    size_t                 altstack_size;
    int                    altstack_installed;
} perl_mt_worker_sig_ctx_t;

void                      Perl_mt_fault_install_signal_handlers(void);
perl_mt_worker_sig_ctx_t *Perl_mt_fault_worker_sig_ctx(void);
int                       Perl_mt_fault_altstack_setup(perl_mt_worker_sig_ctx_t *ctx);
void                      Perl_mt_fault_altstack_teardown(perl_mt_worker_sig_ctx_t *ctx);
const char               *Perl_mt_fault_signal_name(int sig);

void                      Perl_mt_fault_trigger_sigsegv(void);
void                      Perl_mt_fault_trigger_sigbus(void);
void                      Perl_mt_fault_trigger_sigfpe(void);
#if defined(PERL_CORE) || defined(PERL_EXT)
void                      Perl_mt_fault_trigger_oom(pTHX);
#endif

/* Worker state inspection for supervision (mt_worker.c) */
int      Perl_mt_worker_state(uint32_t id);
int      Perl_mt_worker_faulted_sig(uint32_t id);

/* ---- process-wide named endpoint registry --------------------- */

#define MT_NAMED_KIND_CHANNEL  1u
#define MT_NAMED_KIND_POOL     2u

int      Perl_mt_named_register(uint32_t kind, const char *name, uint64_t id,
                                int overwrite, uint64_t *existing_id);
int      Perl_mt_named_lookup(uint32_t kind, const char *name, uint64_t *id_out);
int      Perl_mt_named_unregister(uint32_t kind, const char *name);
#if defined(PERL_CORE) || defined(PERL_EXT)
AV      *Perl_mt_named_list_av(pTHX_ uint32_t kind);
#endif

#endif /* PERL_MT_FAULT_H */

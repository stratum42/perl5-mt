# perl5-mt fault containment and supervision XS glue (included from ext/MT/MT.xs)

MODULE = MT		PACKAGE = MT::Fault

PROTOTYPES: DISABLE

SV *
_last()
    PREINIT:
        extern SV *Perl_mt_fault_last_sv(pTHX);
    CODE:
        RETVAL = Perl_mt_fault_last_sv(aTHX);
    OUTPUT:
        RETVAL

UV
_count()
    PREINIT:
        extern uint64_t Perl_mt_fault_count(void);
    CODE:
        RETVAL = (UV)Perl_mt_fault_count();
    OUTPUT:
        RETVAL

void
_reset()
    PREINIT:
        extern void Perl_mt_fault_reset(void);
    CODE:
        Perl_mt_fault_reset();

SV *
_drain(UV after_seq)
    PREINIT:
        extern AV *Perl_mt_fault_drain_av(pTHX_ uint64_t *after_seq);
        uint64_t seq;
        AV *av;
        AV *pair;
    CODE:
        seq = (uint64_t)after_seq;
        av = Perl_mt_fault_drain_av(aTHX_ &seq);
        pair = newAV();
        av_push(pair, newSVuv((UV)seq));
        av_push(pair, newRV_noinc((SV *)av));
        RETVAL = newRV_noinc((SV *)pair);
    OUTPUT:
        RETVAL

SV *
_sites()
    PREINIT:
        extern AV *Perl_mt_fault_sites_av(pTHX);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_fault_sites_av(aTHX));
    OUTPUT:
        RETVAL

void
_inject_recoverable(const char *msg = "test recoverable fault")
    PREINIT:
        extern void Perl_mt_fault_raise(pTHX_ uint32_t cause, uint32_t severity,
                                        uint32_t unit_kind, uint32_t unit_id,
                                        uint32_t generation, int32_t err_code,
                                        const char *msg);
    CODE:
        Perl_mt_fault_raise(aTHX_ 9u, 1u,
                            PL_mt_worker_id ? 1u : 0u,
                            (uint32_t)PL_mt_worker_id, 1u, 0, msg);

void
_inject_fatal(const char *msg = "test fatal fault")
    PREINIT:
        extern void Perl_mt_fault_raise(pTHX_ uint32_t cause, uint32_t severity,
                                        uint32_t unit_kind, uint32_t unit_id,
                                        uint32_t generation, int32_t err_code,
                                        const char *msg);
    CODE:
        Perl_mt_fault_raise(aTHX_ 10u, 2u,
                            PL_mt_worker_id ? 1u : 0u,
                            (uint32_t)PL_mt_worker_id, 1u, 0, msg);

void
_trigger_sigsegv()
    PREINIT:
        extern void Perl_mt_fault_trigger_sigsegv(void);
    CODE:
        Perl_mt_fault_trigger_sigsegv();

void
_trigger_sigbus()
    PREINIT:
        extern void Perl_mt_fault_trigger_sigbus(void);
    CODE:
        Perl_mt_fault_trigger_sigbus();

void
_trigger_sigfpe()
    PREINIT:
        extern void Perl_mt_fault_trigger_sigfpe(void);
    CODE:
        Perl_mt_fault_trigger_sigfpe();

void
_trigger_oom()
    PREINIT:
        extern void Perl_mt_fault_trigger_oom(pTHX);
    CODE:
        Perl_mt_fault_trigger_oom(aTHX);

MODULE = MT		PACKAGE = MT::Supervisor

PROTOTYPES: DISABLE

UV
_tick()
    PREINIT:
        extern U32 Perl_mt_sup_tick(pTHX);
    CODE:
        RETVAL = (UV)Perl_mt_sup_tick(aTHX);
    OUTPUT:
        RETVAL

SV *
_status()
    PREINIT:
        extern AV *Perl_mt_sup_status_av(pTHX);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_sup_status_av(aTHX));
    OUTPUT:
        RETVAL

void
_beat()
    PREINIT:
        extern int32_t Perl_mt_sup_get_tls_slot(void);
        extern void Perl_mt_sup_beat_slot(int32_t slot);
        int32_t slot;
    CODE:
        slot = Perl_mt_sup_get_tls_slot();
        if (slot >= 0)
            Perl_mt_sup_beat_slot(slot);

void
_pause_heartbeat(int paused, IV worker_id = -1)
    PREINIT:
        extern int32_t Perl_mt_sup_get_tls_slot(void);
        extern int32_t Perl_mt_sup_find_slot(uint32_t unit_kind, uint32_t unit_id);
        extern void Perl_mt_sup_pause_heartbeat(int32_t slot, int paused);
        int32_t slot;
    CODE:
        if (worker_id >= 0)
            slot = Perl_mt_sup_find_slot(1u, (uint32_t)worker_id);
        else
            slot = Perl_mt_sup_get_tls_slot();
        if (slot >= 0)
            Perl_mt_sup_pause_heartbeat(slot, paused);

UV
_bench_beat(UV iters)
    PREINIT:
        extern int32_t Perl_mt_sup_get_tls_slot(void);
        extern uint64_t Perl_mt_sup_bench_beat(int32_t slot, uint64_t iters);
    CODE:
        RETVAL = (UV)Perl_mt_sup_bench_beat(Perl_mt_sup_get_tls_slot(),
                                            (uint64_t)iters);
    OUTPUT:
        RETVAL

SV *
_config()
    PREINIT:
        extern void Perl_mt_sup_config(uint32_t *hb_ms, uint32_t *hb_miss);
        uint32_t hb_ms = 0, hb_miss = 0;
        HV *hv;
    CODE:
        Perl_mt_sup_config(&hb_ms, &hb_miss);
        hv = newHV();
        (void)hv_stores(hv, "heartbeat_ms", newSVuv((UV)hb_ms));
        (void)hv_stores(hv, "heartbeat_miss", newSVuv((UV)hb_miss));
        RETVAL = newRV_noinc((SV *)hv);
    OUTPUT:
        RETVAL

MODULE = MT		PACKAGE = MT::Pool

PROTOTYPES: DISABLE

SV *
_encode_list(...)
    PREINIT:
        extern SV *Perl_mt_pool_encode_list(pTHX_ SV **items, I32 count);
        SV **args;
        I32 i;
    CODE:
        if (items > 0) {
            Newx(args, items, SV *);
            for (i = 0; i < items; i++)
                args[i] = ST(i);
            RETVAL = Perl_mt_pool_encode_list(aTHX_ args, items);
            Safefree(args);
        }
        else {
            RETVAL = Perl_mt_pool_encode_list(aTHX_ NULL, 0);
        }
    OUTPUT:
        RETVAL

void
_decode_list(SV *blob)
    PREINIT:
        extern AV *Perl_mt_pool_decode_list(pTHX_ SV *blob);
        AV *av;
        SSize_t i, n;
    PPCODE:
        av = Perl_mt_pool_decode_list(aTHX_ blob);
        n = av_len(av) + 1;
        EXTEND(SP, n);
        for (i = 0; i < n; i++) {
            SV **elem = av_fetch(av, i, 0);
            PUSHs(elem ? sv_2mortal(SvREFCNT_inc(*elem)) : &PL_sv_undef);
        }
        SvREFCNT_dec((SV *)av);

MODULE = MT		PACKAGE = MT::IO

PROTOTYPES: DISABLE

IV
_test_combined_sleep()
    PREINIT:
        extern int Perl_mt_aio_test_combined_sleep(pTHX);
    CODE:
        RETVAL = (IV)Perl_mt_aio_test_combined_sleep(aTHX);
    OUTPUT:
        RETVAL

MODULE = MT		PACKAGE = MT::Worker

PROTOTYPES: DISABLE

UV
id()
    CODE:
        RETVAL = (UV)PL_mt_worker_id;
    OUTPUT:
        RETVAL

IV
state(UV wid)
    PREINIT:
        extern int Perl_mt_worker_state(uint32_t id);
    CODE:
        RETVAL = (IV)Perl_mt_worker_state((uint32_t)wid);
    OUTPUT:
        RETVAL

IV
faulted_sig(UV wid)
    PREINIT:
        extern int Perl_mt_worker_faulted_sig(uint32_t id);
    CODE:
        RETVAL = (IV)Perl_mt_worker_faulted_sig((uint32_t)wid);
    OUTPUT:
        RETVAL

MODULE = MT		PACKAGE = MT::Channel

PROTOTYPES: DISABLE

void
_named_register(const char *name, UV id, int overwrite = 0)
    PREINIT:
        extern int Perl_mt_named_register(uint32_t kind, const char *name,
                                          uint64_t id, int overwrite,
                                          uint64_t *existing_id);
        uint64_t ex = 0;
        int rc;
    PPCODE:
        rc = Perl_mt_named_register(1u, name, (uint64_t)id, overwrite, &ex);
        EXTEND(SP, 2);
        PUSHs(sv_2mortal(newSViv((IV)rc)));
        PUSHs(sv_2mortal(newSVuv((UV)ex)));

SV *
_named_lookup(const char *name)
    PREINIT:
        extern int Perl_mt_named_lookup(uint32_t kind, const char *name,
                                        uint64_t *id_out);
        uint64_t id = 0;
    CODE:
        if (Perl_mt_named_lookup(1u, name, &id))
            RETVAL = newSVuv((UV)id);
        else
            RETVAL = &PL_sv_undef;
    OUTPUT:
        RETVAL

IV
_named_unregister(const char *name)
    PREINIT:
        extern int Perl_mt_named_unregister(uint32_t kind, const char *name);
    CODE:
        RETVAL = (IV)Perl_mt_named_unregister(1u, name);
    OUTPUT:
        RETVAL

SV *
_named_list()
    PREINIT:
        extern AV *Perl_mt_named_list_av(pTHX_ uint32_t kind);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_named_list_av(aTHX_ 1u));
    OUTPUT:
        RETVAL

MODULE = MT		PACKAGE = MT::Pool

PROTOTYPES: DISABLE

void
_named_register(const char *name, UV id, int overwrite = 0)
    PREINIT:
        extern int Perl_mt_named_register(uint32_t kind, const char *name,
                                          uint64_t id, int overwrite,
                                          uint64_t *existing_id);
        uint64_t ex = 0;
        int rc;
    PPCODE:
        rc = Perl_mt_named_register(2u, name, (uint64_t)id, overwrite, &ex);
        EXTEND(SP, 2);
        PUSHs(sv_2mortal(newSViv((IV)rc)));
        PUSHs(sv_2mortal(newSVuv((UV)ex)));

SV *
_named_lookup(const char *name)
    PREINIT:
        extern int Perl_mt_named_lookup(uint32_t kind, const char *name,
                                        uint64_t *id_out);
        uint64_t id = 0;
    CODE:
        if (Perl_mt_named_lookup(2u, name, &id))
            RETVAL = newSVuv((UV)id);
        else
            RETVAL = &PL_sv_undef;
    OUTPUT:
        RETVAL

IV
_named_unregister(const char *name)
    PREINIT:
        extern int Perl_mt_named_unregister(uint32_t kind, const char *name);
    CODE:
        RETVAL = (IV)Perl_mt_named_unregister(2u, name);
    OUTPUT:
        RETVAL

SV *
_named_list()
    PREINIT:
        extern AV *Perl_mt_named_list_av(pTHX_ uint32_t kind);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_named_list_av(aTHX_ 2u));
    OUTPUT:
        RETVAL

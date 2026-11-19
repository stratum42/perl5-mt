# perl5-mt partitioning and pairs XS glue (included from ext/MT/MT.xs)

MODULE = MT		PACKAGE = MT::SharedImage

PROTOTYPES: DISABLE

SV *
_build_id()
    PREINIT:
        extern void Perl_mt_shimg_build_id(char *buf, size_t len);
        char buf[64];
    CODE:
        Perl_mt_shimg_build_id(buf, sizeof(buf));
        RETVAL = newSVpv(buf, 0);
    OUTPUT:
        RETVAL

SV *
_create(const char *path, UV fixed_addr, const char *manifest, SV *payload)
    PREINIT:
        extern HV *Perl_mt_shimg_create_hv(pTHX_ const char *path, UV fixed_addr,
                                           const char *manifest, SV *payload_sv);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_shimg_create_hv(aTHX_ path, fixed_addr,
                                                           manifest, payload));
    OUTPUT:
        RETVAL

SV *
_attach(const char *path, const char *expected_manifest = "", UV force_fallback = 0)
    PREINIT:
        extern HV *Perl_mt_shimg_attach_hv(pTHX_ const char *path,
                                           const char *expected_manifest,
                                           UV force_fallback);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_shimg_attach_hv(aTHX_ path,
                                                           expected_manifest,
                                                           force_fallback));
    OUTPUT:
        RETVAL

SV *
_validate(const char *path, const char *expected_manifest = "")
    PREINIT:
        extern HV *Perl_mt_shimg_validate_hv(pTHX_ const char *path,
                                             const char *expected_manifest);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_shimg_validate_hv(aTHX_ path,
                                                             expected_manifest));
    OUTPUT:
        RETVAL

SV *
_payload(UV id)
    PREINIT:
        extern SV *Perl_mt_shimg_payload_sv(pTHX_ UV id);
    CODE:
        RETVAL = Perl_mt_shimg_payload_sv(aTHX_ id);
    OUTPUT:
        RETVAL

SV *
_info(UV id)
    PREINIT:
        extern HV *Perl_mt_shimg_info_hv(pTHX_ UV id);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_shimg_info_hv(aTHX_ id));
    OUTPUT:
        RETVAL

IV
_detach(UV id)
    PREINIT:
        extern int Perl_mt_shimg_detach_id(UV id);
    CODE:
        RETVAL = (IV)Perl_mt_shimg_detach_id(id);
    OUTPUT:
        RETVAL

MODULE = MT		PACKAGE = MT::Namespace

PROTOTYPES: DISABLE

SV *
_caps()
    PREINIT:
        extern HV *Perl_mt_ns_caps_hv(pTHX);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_ns_caps_hv(aTHX));
    OUTPUT:
        RETVAL

UV
_create(const char *name, const char *cwd, UV umask_val, UV cap_mask)
    PREINIT:
        extern UV Perl_mt_ns_create(const char *name, const char *cwd,
                                    uint32_t umask_val, uint64_t cap_mask);
    CODE:
        RETVAL = Perl_mt_ns_create(name, cwd, (uint32_t)umask_val, (uint64_t)cap_mask);
    OUTPUT:
        RETVAL

IV
_destroy(UV id)
    PREINIT:
        extern int Perl_mt_ns_destroy(UV id);
    CODE:
        RETVAL = (IV)Perl_mt_ns_destroy(id);
    OUTPUT:
        RETVAL

IV
_enter(UV id, int isolate_os = 0)
    PREINIT:
        extern int Perl_mt_ns_enter(UV id, int isolate_os);
    CODE:
        RETVAL = (IV)Perl_mt_ns_enter(id, isolate_os);
    OUTPUT:
        RETVAL

void
_leave()
    PREINIT:
        extern void Perl_mt_ns_leave(void);
    CODE:
        Perl_mt_ns_leave();

UV
_current()
    PREINIT:
        extern UV Perl_mt_ns_current(void);
    CODE:
        RETVAL = Perl_mt_ns_current();
    OUTPUT:
        RETVAL

SV *
_info(UV id)
    PREINIT:
        extern HV *Perl_mt_ns_info_hv(pTHX_ UV id);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_ns_info_hv(aTHX_ id));
    OUTPUT:
        RETVAL

MODULE = MT		PACKAGE = MT::Class

PROTOTYPES: DISABLE

UV
_create(const char *name, UV cpu_weight, UV mem_limit_bytes, UV io_budget_ops, UV io_window_ms = 1000)
    PREINIT:
        extern UV Perl_mt_class_create(const char *name, uint32_t cpu_weight,
                                       uint64_t mem_limit_bytes, uint64_t io_budget_ops,
                                       uint64_t io_window_ms);
    CODE:
        RETVAL = Perl_mt_class_create(name, (uint32_t)cpu_weight,
                                      (uint64_t)mem_limit_bytes,
                                      (uint64_t)io_budget_ops,
                                      (uint64_t)io_window_ms);
    OUTPUT:
        RETVAL

IV
_destroy(UV id)
    PREINIT:
        extern int Perl_mt_class_destroy(UV id);
    CODE:
        RETVAL = (IV)Perl_mt_class_destroy(id);
    OUTPUT:
        RETVAL

IV
_bind_thread(UV id)
    PREINIT:
        extern int Perl_mt_class_bind_thread(UV id);
    CODE:
        RETVAL = (IV)Perl_mt_class_bind_thread(id);
    OUTPUT:
        RETVAL

IV
_charge_mem(UV id, UV bytes)
    PREINIT:
        extern int Perl_mt_class_charge_mem(UV id, uint64_t bytes);
    CODE:
        RETVAL = (IV)Perl_mt_class_charge_mem(id, (uint64_t)bytes);
    OUTPUT:
        RETVAL

IV
_release_mem(UV id, UV bytes)
    PREINIT:
        extern int Perl_mt_class_release_mem(UV id, uint64_t bytes);
    CODE:
        RETVAL = (IV)Perl_mt_class_release_mem(id, (uint64_t)bytes);
    OUTPUT:
        RETVAL

IV
_charge_io(UV id, UV ops = 1)
    PREINIT:
        extern int Perl_mt_class_charge_io(UV id, uint64_t ops);
    CODE:
        RETVAL = (IV)Perl_mt_class_charge_io(id, (uint64_t)ops);
    OUTPUT:
        RETVAL

IV
_charge_cpu(UV id, UV work_units = 1)
    PREINIT:
        extern int Perl_mt_class_charge_cpu(UV id, uint64_t work_units);
    CODE:
        RETVAL = (IV)Perl_mt_class_charge_cpu(id, (uint64_t)work_units);
    OUTPUT:
        RETVAL

SV *
_stats(UV id)
    PREINIT:
        extern HV *Perl_mt_class_stats_hv(pTHX_ UV id);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_class_stats_hv(aTHX_ id));
    OUTPUT:
        RETVAL

MODULE = MT		PACKAGE = MT::ProcessPair

PROTOTYPES: DISABLE

UV
_open(const char *path, const char *name, const char *image_path, UV hb_timeout_ms, int is_supervisor, int create)
    PREINIT:
        extern UV Perl_mt_pair_open(const char *path, const char *name,
                                    const char *image_path, uint32_t hb_timeout_ms,
                                    int is_supervisor, int create);
    CODE:
        RETVAL = Perl_mt_pair_open(path, name, image_path,
                                   (uint32_t)hb_timeout_ms,
                                   is_supervisor, create);
    OUTPUT:
        RETVAL

IV
_close(UV id)
    PREINIT:
        extern int Perl_mt_pair_close(UV id);
    CODE:
        RETVAL = (IV)Perl_mt_pair_close(id);
    OUTPUT:
        RETVAL

IV
_register(UV id, UV role)
    PREINIT:
        extern int Perl_mt_pair_register(UV id, uint32_t role);
    CODE:
        RETVAL = (IV)Perl_mt_pair_register(id, (uint32_t)role);
    OUTPUT:
        RETVAL

IV
_heartbeat(UV id, UV role)
    PREINIT:
        extern int Perl_mt_pair_heartbeat(UV id, uint32_t role);
    CODE:
        RETVAL = (IV)Perl_mt_pair_heartbeat(id, (uint32_t)role);
    OUTPUT:
        RETVAL

UV
_checkpoint(UV id, SV *data_sv)
    PREINIT:
        extern uint64_t Perl_mt_pair_checkpoint(UV id, const void *data, size_t len);
        STRLEN len = 0;
        const char *p;
    CODE:
        p = SvPV_const(data_sv, len);
        RETVAL = (UV)Perl_mt_pair_checkpoint(id, p, (size_t)len);
    OUTPUT:
        RETVAL

SV *
_last_checkpoint(UV id)
    PREINIT:
        extern HV *Perl_mt_pair_last_checkpoint_hv(pTHX_ UV id);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_pair_last_checkpoint_hv(aTHX_ id));
    OUTPUT:
        RETVAL

IV
_check_failover(UV id)
    PREINIT:
        extern int Perl_mt_pair_check_failover(UV id);
    CODE:
        RETVAL = (IV)Perl_mt_pair_check_failover(id);
    OUTPUT:
        RETVAL

SV *
_status(UV id)
    PREINIT:
        extern HV *Perl_mt_pair_status_hv(pTHX_ UV id);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_pair_status_hv(aTHX_ id));
    OUTPUT:
        RETVAL

IV
_tx_commit(UV id, SV *href)
    PREINIT:
        extern int Perl_mt_pair_tx_commit(pTHX_ UV id, HV *kv_hv);
    CODE:
        if (!SvROK(href) || SvTYPE(SvRV(href)) != SVt_PVHV)
            Perl_croak(aTHX_ "MT::ProcessPair::_tx_commit: expected hashref");
        RETVAL = (IV)Perl_mt_pair_tx_commit(aTHX_ id, (HV *)SvRV(href));
    OUTPUT:
        RETVAL

SV *
_tx_snapshot(UV id)
    PREINIT:
        extern HV *Perl_mt_pair_tx_snapshot_hv(pTHX_ UV id);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_pair_tx_snapshot_hv(aTHX_ id));
    OUTPUT:
        RETVAL

MODULE = MT		PACKAGE = MT::PDLBridge

PROTOTYPES: DISABLE

UV
_ndarray_create(UV dtype, UV nelem, SV *dims_avref = &PL_sv_undef)
    PREINIT:
        extern UV Perl_mt_ndarray_create(uint32_t dtype, UV nelem,
                                         const uint64_t *dims, uint32_t ndims);
        uint64_t dims[8];
        uint32_t ndims = 0;
    CODE:
        if (SvROK(dims_avref) && SvTYPE(SvRV(dims_avref)) == SVt_PVAV) {
            AV *av = (AV *)SvRV(dims_avref);
            SSize_t n = av_len(av) + 1;
            SSize_t i;
            if (n > 8) n = 8;
            for (i = 0; i < n; i++) {
                SV **svp = av_fetch(av, i, 0);
                dims[i] = (svp && *svp) ? (uint64_t)SvUV(*svp) : 1;
            }
            ndims = (uint32_t)n;
        }
        RETVAL = Perl_mt_ndarray_create((uint32_t)dtype, nelem,
                                        ndims ? dims : NULL, ndims);
    OUTPUT:
        RETVAL

IV
_ndarray_destroy(UV id)
    PREINIT:
        extern int Perl_mt_ndarray_destroy(UV id);
    CODE:
        RETVAL = (IV)Perl_mt_ndarray_destroy(id);
    OUTPUT:
        RETVAL

IV
_ndarray_seal(UV id)
    PREINIT:
        extern int Perl_mt_ndarray_seal(UV id);
    CODE:
        RETVAL = (IV)Perl_mt_ndarray_seal(id);
    OUTPUT:
        RETVAL

IV
_ndarray_is_sealed(UV id)
    PREINIT:
        extern int Perl_mt_ndarray_is_sealed(UV id);
    CODE:
        RETVAL = (IV)Perl_mt_ndarray_is_sealed(id);
    OUTPUT:
        RETVAL

IV
_ndarray_set_f64(UV id, UV idx, NV val)
    PREINIT:
        extern int Perl_mt_ndarray_set_f64(UV id, UV idx, double val);
    CODE:
        RETVAL = (IV)Perl_mt_ndarray_set_f64(id, idx, (double)val);
    OUTPUT:
        RETVAL

NV
_ndarray_get_f64(UV id, UV idx)
    PREINIT:
        extern double Perl_mt_ndarray_get_f64(UV id, UV idx);
    CODE:
        RETVAL = (NV)Perl_mt_ndarray_get_f64(id, idx);
    OUTPUT:
        RETVAL

IV
_ndarray_fill_seq(UV id, NV start, NV step)
    PREINIT:
        extern int Perl_mt_ndarray_fill_seq(UV id, double start, double step);
    CODE:
        RETVAL = (IV)Perl_mt_ndarray_fill_seq(id, (double)start, (double)step);
    OUTPUT:
        RETVAL

IV
_ndarray_kernel_slice(UV src_id, UV dst_id, UV start, UV len, NV alpha, NV beta)
    PREINIT:
        extern int Perl_mt_ndarray_kernel_slice(UV src_id, UV dst_id, UV start, UV len,
                                                double alpha, double beta);
    CODE:
        RETVAL = (IV)Perl_mt_ndarray_kernel_slice(src_id, dst_id, start, len,
                                                  (double)alpha, (double)beta);
    OUTPUT:
        RETVAL

NV
_ndarray_sum_slice(UV id, UV start, UV len)
    PREINIT:
        extern double Perl_mt_ndarray_sum_slice(UV id, UV start, UV len);
    CODE:
        RETVAL = (NV)Perl_mt_ndarray_sum_slice(id, start, len);
    OUTPUT:
        RETVAL

NV
_ndarray_dot_slice(UV a_id, UV b_id, UV start, UV len)
    PREINIT:
        extern double Perl_mt_ndarray_dot_slice(UV a_id, UV b_id, UV start, UV len);
    CODE:
        RETVAL = (NV)Perl_mt_ndarray_dot_slice(a_id, b_id, start, len);
    OUTPUT:
        RETVAL

SV *
_ndarray_slice_be_bytes(UV id, UV start, UV len)
    PREINIT:
        extern SV *Perl_mt_ndarray_slice_be_bytes(pTHX_ UV id, UV start, UV len);
    CODE:
        RETVAL = Perl_mt_ndarray_slice_be_bytes(aTHX_ id, start, len);
    OUTPUT:
        RETVAL

SV *
_ndarray_info(UV id)
    PREINIT:
        extern HV *Perl_mt_ndarray_info_hv(pTHX_ UV id);
    CODE:
        RETVAL = newRV_noinc((SV *)Perl_mt_ndarray_info_hv(aTHX_ id));
    OUTPUT:
        RETVAL

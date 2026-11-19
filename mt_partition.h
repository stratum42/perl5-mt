/*    mt_partition.h
 *
 *    perl5-mt: partitioning, multi-process shared images,
 *    worker-group namespaces, resource classes, cross-process pairs
 *    with checkpoint ring & supervisor failover, and threadsafe
 *    shm-backed ndarray storage for PDL bridge.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_MT_PARTITION_H
#define PERL_MT_PARTITION_H

#include <stddef.h>
#include <stdint.h>
#include "krt/krt.h"
#include "krt/krt_atomic.h"

/* ---- 10a: Shared Image File (LPAR tier) --------------------------------- */

#define MT_SHIMG_MAGIC            0x4D54494Du  /* 'MTIM' */
#define MT_SHIMG_VERSION          1u
#define MT_SHIMG_DEFAULT_ADDR     0x500000000000ULL
#define MT_SHIMG_BUILD_ID_MAX     64u
#define MT_SHIMG_MANIFEST_MAX     512u
#define MT_SHIMG_MAX_HANDLES      64u

typedef struct perl_mt_shimg_hdr {
    uint32_t magic;
    uint32_t version;
    uint64_t fixed_addr;
    uint64_t file_size;
    uint64_t payload_offset;
    uint64_t payload_len;
    uint64_t module_hash;
    uint64_t payload_checksum;
    char     build_id[MT_SHIMG_BUILD_ID_MAX];
    char     manifest[MT_SHIMG_MANIFEST_MAX];
} perl_mt_shimg_hdr_t;

typedef struct perl_mt_shimg_info {
    uint32_t id;
    int      active;
    int      fd;
    void    *base;
    size_t   map_len;
    uint64_t requested_addr;
    uint64_t mapped_addr;
    int      exact_addr_match;
    int      fallback;
    int      pkey_armed;
    int      mprotect_armed;
    perl_krt_key_t pkey;
    uint64_t payload_offset;
    uint64_t payload_len;
    uint64_t module_hash;
    uint64_t payload_checksum;
    char     build_id[MT_SHIMG_BUILD_ID_MAX];
    char     manifest[MT_SHIMG_MANIFEST_MAX];
} perl_mt_shimg_info_t;

/* ---- 10b: Worker-Group Namespaces (WPAR tier) --------------------------- */

#define MT_NS_MAX                 64u
#define MT_NS_NAME_MAX            64u
#define MT_NS_PATH_MAX            512u
#define MT_NS_FAULT_MODEL         "shared interpreter is the kernel"

/* ---- 10c: Resource Classes (WLM tier) ----------------------------------- */

#define MT_CLASS_MAX              64u
#define MT_CLASS_NAME_MAX         64u

/* ---- 10e: Cross-Process Pairs & Supervisor Pair ------------------------- */

#define MT_PAIR_MAGIC             0x4D545052u  /* 'MTPR' */
#define MT_PAIR_VERSION           1u
#define MT_PAIR_RING_SLOTS        16u
#define MT_PAIR_CKPT_DATA_MAX     4096u
#define MT_PAIR_MAX_HANDLES       32u
#define MT_PAIR_KV_MAX            64u
#define MT_PAIR_KEY_MAX           64u
#define MT_PAIR_VAL_MAX           192u

#define MT_PAIR_ROLE_NONE         0u
#define MT_PAIR_ROLE_PRIMARY      1u
#define MT_PAIR_ROLE_BACKUP       2u

#define MT_PAIR_STATE_INIT        0u
#define MT_PAIR_STATE_ACTIVE      1u
#define MT_PAIR_STATE_FAILOVER    2u
#define MT_PAIR_STATE_STOPPED     3u

typedef struct perl_mt_pair_ckpt {
    perl_krt_au64_t seq;
    uint64_t        timestamp_ns;
    uint32_t        data_len;
    uint32_t        checksum;
    uint8_t         data[MT_PAIR_CKPT_DATA_MAX];
} perl_mt_pair_ckpt_t;

typedef struct perl_mt_pair_kv {
    char     key[MT_PAIR_KEY_MAX];
    char     val[MT_PAIR_VAL_MAX];
    uint64_t version;
    uint32_t used;
    uint32_t pad;
} perl_mt_pair_kv_t;

typedef struct perl_mt_pair_shm {
    uint32_t            magic;
    uint32_t            version;
    perl_krt_au32_t     primary_pid;
    perl_krt_au32_t     backup_pid;
    perl_krt_au32_t     active_role;
    perl_krt_au32_t     epoch;
    perl_krt_au32_t     state;
    uint32_t            hb_timeout_ms;
    perl_krt_au64_t     primary_hb_ns;
    perl_krt_au64_t     backup_hb_ns;
    perl_krt_au64_t     checkpoint_seq;
    perl_krt_au32_t     ring_head;
    perl_krt_au32_t     tx_lock;
    perl_krt_au64_t     tx_epoch;
    uint32_t            kv_count;
    uint32_t            is_supervisor;
    char                name[64];
    char                image_path[256];
    perl_mt_pair_ckpt_t ring[MT_PAIR_RING_SLOTS];
    perl_mt_pair_kv_t   kv[MT_PAIR_KV_MAX];
} perl_mt_pair_shm_t;

/* ---- 10f: Threadsafe SHM-backed NDArray (PDL Bridge) -------------------- */

#define MT_NDARRAY_MAX            128u
#define MT_NDARRAY_DTYPE_F64      1u
#define MT_NDARRAY_DTYPE_I64      2u
#define MT_NDARRAY_MAX_DIMS       8u

/* ---- C API exposed to XS (ext/MT/mt_phase10.xs.inc) --------------------- */

#if defined(PERL_CORE) || defined(PERL_EXT)

/* 10a: Shared Image File */
void  Perl_mt_shimg_build_id(char *buf, size_t len);
HV   *Perl_mt_shimg_create_hv(pTHX_ const char *path, UV fixed_addr,
                              const char *manifest, SV *payload_sv);
HV   *Perl_mt_shimg_attach_hv(pTHX_ const char *path,
                              const char *expected_manifest,
                              UV force_fallback);
SV   *Perl_mt_shimg_payload_sv(pTHX_ UV id);
HV   *Perl_mt_shimg_info_hv(pTHX_ UV id);
int   Perl_mt_shimg_detach_id(UV id);
HV   *Perl_mt_shimg_validate_hv(pTHX_ const char *path,
                                const char *expected_manifest);

/* 10b: Worker-Group Namespaces */
HV   *Perl_mt_ns_caps_hv(pTHX);
UV    Perl_mt_ns_create(const char *name, const char *cwd,
                        uint32_t umask_val, uint64_t cap_mask);
int   Perl_mt_ns_destroy(UV id);
int   Perl_mt_ns_enter(UV id, int isolate_os);
void  Perl_mt_ns_leave(void);
UV    Perl_mt_ns_current(void);
HV   *Perl_mt_ns_info_hv(pTHX_ UV id);

/* 10c: Resource Classes */
UV    Perl_mt_class_create(const char *name, uint32_t cpu_weight,
                           uint64_t mem_limit_bytes, uint64_t io_budget_ops,
                           uint64_t io_window_ms);
int   Perl_mt_class_destroy(UV id);
int   Perl_mt_class_bind_thread(UV id);
int   Perl_mt_class_charge_mem(UV id, uint64_t bytes);
int   Perl_mt_class_release_mem(UV id, uint64_t bytes);
int   Perl_mt_class_charge_io(UV id, uint64_t ops);
int   Perl_mt_class_charge_cpu(UV id, uint64_t work_units);
HV   *Perl_mt_class_stats_hv(pTHX_ UV id);

/* 10e: Cross-Process Pairs & Supervisor Pair */
UV    Perl_mt_pair_open(const char *path, const char *name,
                        const char *image_path, uint32_t hb_timeout_ms,
                        int is_supervisor, int create);
int   Perl_mt_pair_close(UV id);
int   Perl_mt_pair_register(UV id, uint32_t role);
int   Perl_mt_pair_heartbeat(UV id, uint32_t role);
uint64_t Perl_mt_pair_checkpoint(UV id, const void *data, size_t len);
HV   *Perl_mt_pair_last_checkpoint_hv(pTHX_ UV id);
int   Perl_mt_pair_check_failover(UV id);
HV   *Perl_mt_pair_status_hv(pTHX_ UV id);
int   Perl_mt_pair_tx_commit(pTHX_ UV id, HV *kv_hv);
HV   *Perl_mt_pair_tx_snapshot_hv(pTHX_ UV id);

/* 10f: Threadsafe SHM-backed NDArray (PDL Bridge) */
UV    Perl_mt_ndarray_create(uint32_t dtype, UV nelem,
                             const uint64_t *dims, uint32_t ndims);
int   Perl_mt_ndarray_destroy(UV id);
int   Perl_mt_ndarray_seal(UV id);
int   Perl_mt_ndarray_is_sealed(UV id);
int   Perl_mt_ndarray_set_f64(UV id, UV idx, double val);
double Perl_mt_ndarray_get_f64(UV id, UV idx);
int   Perl_mt_ndarray_fill_seq(UV id, double start, double step);
int   Perl_mt_ndarray_kernel_slice(UV src_id, UV dst_id, UV start, UV len,
                                   double alpha, double beta);
double Perl_mt_ndarray_sum_slice(UV id, UV start, UV len);
double Perl_mt_ndarray_dot_slice(UV a_id, UV b_id, UV start, UV len);
SV   *Perl_mt_ndarray_slice_be_bytes(pTHX_ UV id, UV start, UV len);
HV   *Perl_mt_ndarray_info_hv(pTHX_ UV id);

#endif /* PERL_CORE || PERL_EXT */

#endif /* PERL_MT_PARTITION_H */

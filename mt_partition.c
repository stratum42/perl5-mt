/*    mt_partition.c
 *
 *    perl5-mt: partitioning, multi-process shared images,
 *    worker-group namespaces, resource classes, cross-process
 *    pairs with checkpoint ring & supervisor failover, and threadsafe
 *    shm-backed ndarray storage for the PDL bridge.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#include "EXTERN.h"
#define PERL_IN_MT_PARTITION_C
#include "perl.h"
#include "mt_partition.h"
#include "krt/krt.h"
#include "krt/krt_atomic.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#  include <pthread/qos.h>
#endif

#if defined(__linux__)
#  include <sched.h>
#endif

/* ---- shared utilities --------------------------------------------------- */

static uint64_t
S_fnv1a_64(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 14695981039346656037ULL;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint32_t
S_fnv1a_32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261U;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619U;
    }
    return h;
}

static size_t
S_round_up_page(size_t n)
{
    size_t pg = perl_krt_page_size();
    if (pg == 0) pg = 4096;
    return (n + pg - 1) & ~(pg - 1);
}

static void
S_copy_str(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

/* =========================================================================
 * 10a: Shared Image File (LPAR tier)
 * ========================================================================= */

static pthread_mutex_t      S_shimg_lock = PTHREAD_MUTEX_INITIALIZER;
static perl_mt_shimg_info_t S_shimg_slots[MT_SHIMG_MAX_HANDLES];
static uint32_t             S_shimg_next_id = 1;

void
Perl_mt_shimg_build_id(char *buf, size_t len)
{
    const char *arch = "unknown";
#if defined(__aarch64__) || defined(__arm64__)
    arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    arch = "x86_64";
#elif defined(__powerpc64__) || defined(__ppc64__)
    arch = PERL_KRT_BIG_ENDIAN ? "ppc64be" : "ppc64le";
#elif defined(__s390x__)
    arch = "s390x";
#endif
    snprintf(buf, len, "perl-%d.%d.%d-%s-%s-%s",
             (int)PERL_REVISION, (int)PERL_VERSION, (int)PERL_SUBVERSION,
             arch,
             (sizeof(void *) == 8) ? "lp64" : "ilp32",
             PERL_KRT_BIG_ENDIAN ? "be" : "le");
}

static int
S_shimg_alloc_slot(uint32_t *out_idx)
{
    uint32_t i;
    for (i = 0; i < MT_SHIMG_MAX_HANDLES; i++) {
        if (!S_shimg_slots[i].active) {
            memset(&S_shimg_slots[i], 0, sizeof(S_shimg_slots[i]));
            S_shimg_slots[i].active = 1;
            S_shimg_slots[i].fd = -1;
            S_shimg_slots[i].pkey = PERL_KRT_KEY_NONE;
            S_shimg_slots[i].id = S_shimg_next_id++;
            if (S_shimg_next_id == 0) S_shimg_next_id = 1;
            *out_idx = i;
            return 0;
        }
    }
    return -ENOSPC;
}

static perl_mt_shimg_info_t *
S_shimg_find_locked(UV id)
{
    uint32_t i;
    for (i = 0; i < MT_SHIMG_MAX_HANDLES; i++) {
        if (S_shimg_slots[i].active && (UV)S_shimg_slots[i].id == id)
            return &S_shimg_slots[i];
    }
    return NULL;
}

static HV *
S_shimg_to_hv(pTHX_ const perl_mt_shimg_info_t *info)
{
    HV *hv = newHV();
    (void)hv_stores(hv, "id",               newSVuv((UV)info->id));
    (void)hv_stores(hv, "requested_addr",   newSVuv((UV)info->requested_addr));
    (void)hv_stores(hv, "mapped_addr",      newSVuv((UV)info->mapped_addr));
    (void)hv_stores(hv, "map_len",          newSVuv((UV)info->map_len));
    (void)hv_stores(hv, "exact_addr_match", newSViv(info->exact_addr_match ? 1 : 0));
    (void)hv_stores(hv, "fallback",         newSViv(info->fallback ? 1 : 0));
    (void)hv_stores(hv, "pkey_armed",       newSViv(info->pkey_armed ? 1 : 0));
    (void)hv_stores(hv, "mprotect_armed",   newSViv(info->mprotect_armed ? 1 : 0));
    (void)hv_stores(hv, "payload_offset",   newSVuv((UV)info->payload_offset));
    (void)hv_stores(hv, "payload_len",      newSVuv((UV)info->payload_len));
    (void)hv_stores(hv, "module_hash",      newSVuv((UV)info->module_hash));
    (void)hv_stores(hv, "payload_checksum", newSVuv((UV)info->payload_checksum));
    (void)hv_stores(hv, "build_id",         newSVpv(info->build_id, 0));
    (void)hv_stores(hv, "manifest",         newSVpv(info->manifest, 0));
    return hv;
}

static void *
S_shimg_mmap_try(int fd, size_t map_len, int prot, int flags,
                 uint64_t req_addr, int force_fallback,
                 int *exact_out, int *fallback_out)
{
    void *base = MAP_FAILED;
    *exact_out = 0;
    *fallback_out = 0;

    if (!force_fallback && req_addr != 0) {
        void *hint = (void *)(uintptr_t)req_addr;
#ifdef MAP_FIXED_NOREPLACE
        base = mmap(hint, map_len, prot, flags | MAP_FIXED_NOREPLACE, fd, 0);
        if (base != MAP_FAILED && (uintptr_t)base == (uintptr_t)req_addr) {
            *exact_out = 1;
            return base;
        }
        if (base != MAP_FAILED) {
            munmap(base, map_len);
            base = MAP_FAILED;
        }
#else
        base = mmap(hint, map_len, prot, flags, fd, 0);
        if (base != MAP_FAILED) {
            if ((uintptr_t)base == (uintptr_t)req_addr) {
                *exact_out = 1;
                return base;
            }
            /* OS ignored hint because address was unavailable: keep mapping as fallback */
            *fallback_out = 1;
            return base;
        }
#endif
    }

    base = mmap(NULL, map_len, prot, flags, fd, 0);
    if (base != MAP_FAILED) {
        if (req_addr != 0 && (uintptr_t)base == (uintptr_t)req_addr)
            *exact_out = 1;
        else
            *fallback_out = 1;
    }
    return base;
}

static void
S_shimg_arm_protection(perl_mt_shimg_info_t *info)
{
    perl_krt_key_t key = PERL_KRT_KEY_NONE;
    info->pkey_armed = 0;
    info->mprotect_armed = 0;
    info->pkey = PERL_KRT_KEY_NONE;

    if (perl_krt_key_alloc(&key) == 0 && key >= 0) {
        perl_krt_shm_t fake;
        memset(&fake, 0, sizeof(fake));
        fake.base = info->base;
        fake.len  = info->map_len;
        if (perl_krt_shm_bind_key(&fake, key) == 0 &&
            perl_krt_key_arm(key, PERL_KRT_KEY_NOWRITE) == 0) {
            info->pkey = key;
            info->pkey_armed = 1;
        } else {
            perl_krt_key_free(key);
        }
    }

    if (mprotect(info->base, info->map_len, PROT_READ) == 0) {
        info->mprotect_armed = 1;
    }
}

HV *
Perl_mt_shimg_create_hv(pTHX_ const char *path, UV fixed_addr,
                        const char *manifest, SV *payload_sv)
{
    STRLEN payload_len = 0;
    const char *payload = NULL;
    size_t hdr_size, total_len;
    int fd;
    void *base;
    int exact = 0, fallback = 0;
    perl_mt_shimg_hdr_t hdr;
    uint32_t idx;
    perl_mt_shimg_info_t *info;
    HV *hv;

    if (!path || !*path)
        Perl_croak(aTHX_ "MT::SharedImage: path must be non-empty");

    if (payload_sv && SvOK(payload_sv))
        payload = SvPV_const(payload_sv, payload_len);
    else
        payload = "";

    hdr_size = (sizeof(perl_mt_shimg_hdr_t) + 63u) & ~(size_t)63u;
    total_len = S_round_up_page(hdr_size + (size_t)payload_len);
    if (fixed_addr == 0)
        fixed_addr = (UV)MT_SHIMG_DEFAULT_ADDR;

    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        Perl_croak(aTHX_ "MT::SharedImage: open(%s) failed: %s", path, strerror(errno));

    if (ftruncate(fd, (off_t)total_len) < 0) {
        int e = errno;
        close(fd);
        Perl_croak(aTHX_ "MT::SharedImage: ftruncate(%s) failed: %s", path, strerror(e));
    }

    base = S_shimg_mmap_try(fd, total_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                            (uint64_t)fixed_addr, 0, &exact, &fallback);
    if (base == MAP_FAILED) {
        int e = errno;
        close(fd);
        Perl_croak(aTHX_ "MT::SharedImage: mmap(%s) failed: %s", path, strerror(e));
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic            = MT_SHIMG_MAGIC;
    hdr.version          = MT_SHIMG_VERSION;
    hdr.fixed_addr       = (uint64_t)(uintptr_t)base;
    hdr.file_size        = (uint64_t)total_len;
    hdr.payload_offset   = (uint64_t)hdr_size;
    hdr.payload_len      = (uint64_t)payload_len;
    hdr.module_hash      = S_fnv1a_64(manifest ? manifest : "",
                                      manifest ? strlen(manifest) : 0);
    hdr.payload_checksum = S_fnv1a_64(payload, (size_t)payload_len);
    Perl_mt_shimg_build_id(hdr.build_id, sizeof(hdr.build_id));
    S_copy_str(hdr.manifest, sizeof(hdr.manifest), manifest ? manifest : "");

    memcpy(base, &hdr, sizeof(hdr));
    if (payload_len > 0)
        memcpy((uint8_t *)base + hdr_size, payload, (size_t)payload_len);
    (void)msync(base, total_len, MS_SYNC);

    pthread_mutex_lock(&S_shimg_lock);
    if (S_shimg_alloc_slot(&idx) != 0) {
        pthread_mutex_unlock(&S_shimg_lock);
        munmap(base, total_len);
        close(fd);
        Perl_croak(aTHX_ "MT::SharedImage: handle table full");
    }
    info = &S_shimg_slots[idx];
    info->fd               = fd;
    info->base             = base;
    info->map_len          = total_len;
    info->requested_addr   = (uint64_t)fixed_addr;
    info->mapped_addr      = (uint64_t)(uintptr_t)base;
    info->exact_addr_match = exact;
    info->fallback         = fallback;
    info->payload_offset   = hdr.payload_offset;
    info->payload_len      = hdr.payload_len;
    info->module_hash      = hdr.module_hash;
    info->payload_checksum = hdr.payload_checksum;
    S_copy_str(info->build_id, sizeof(info->build_id), hdr.build_id);
    S_copy_str(info->manifest, sizeof(info->manifest), hdr.manifest);
    S_shimg_arm_protection(info);

    hv = S_shimg_to_hv(aTHX_ info);
    pthread_mutex_unlock(&S_shimg_lock);
    return hv;
}

static int
S_shimg_validate_hdr(const perl_mt_shimg_hdr_t *hdr, size_t file_size,
                     const char *expected_manifest,
                     char *errbuf, size_t errlen)
{
    char cur_build[MT_SHIMG_BUILD_ID_MAX];

    if (hdr->magic != MT_SHIMG_MAGIC) {
        snprintf(errbuf, errlen, "bad magic 0x%08x (expected 0x%08x)",
                 hdr->magic, MT_SHIMG_MAGIC);
        return -EINVAL;
    }
    if (hdr->version != MT_SHIMG_VERSION) {
        snprintf(errbuf, errlen, "unsupported version %u (expected %u)",
                 hdr->version, MT_SHIMG_VERSION);
        return -EINVAL;
    }
    if (hdr->file_size > (uint64_t)file_size ||
        hdr->payload_offset < sizeof(perl_mt_shimg_hdr_t) ||
        hdr->payload_offset + hdr->payload_len > (uint64_t)file_size) {
        snprintf(errbuf, errlen, "corrupt header bounds (file_size=%zu)", file_size);
        return -EINVAL;
    }
    Perl_mt_shimg_build_id(cur_build, sizeof(cur_build));
    if (strncmp(hdr->build_id, cur_build, sizeof(hdr->build_id)) != 0) {
        snprintf(errbuf, errlen, "perl build_id mismatch: image='%s' runtime='%s'",
                 hdr->build_id, cur_build);
        return -EPROTO;
    }
    if (expected_manifest && *expected_manifest) {
        uint64_t want_hash = S_fnv1a_64(expected_manifest, strlen(expected_manifest));
        if (hdr->module_hash != want_hash ||
            strncmp(hdr->manifest, expected_manifest, sizeof(hdr->manifest)) != 0) {
            snprintf(errbuf, errlen, "module manifest mismatch (image invalidated)");
            return -ESTALE;
        }
    }
    return 0;
}

HV *
Perl_mt_shimg_validate_hv(pTHX_ const char *path, const char *expected_manifest)
{
    HV *hv = newHV();
    int fd;
    struct stat st;
    perl_mt_shimg_hdr_t hdr;
    char errbuf[256];
    ssize_t n;

    errbuf[0] = '\0';
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(errbuf, sizeof(errbuf), "open failed: %s", strerror(errno));
        (void)hv_stores(hv, "valid", newSViv(0));
        (void)hv_stores(hv, "reason", newSVpv(errbuf, 0));
        return hv;
    }
    if (fstat(fd, &st) < 0 || (size_t)st.st_size < sizeof(hdr)) {
        close(fd);
        (void)hv_stores(hv, "valid", newSViv(0));
        (void)hv_stores(hv, "reason", newSVpvs("file too small for image header"));
        return hv;
    }
    n = pread(fd, &hdr, sizeof(hdr), 0);
    if (n != (ssize_t)sizeof(hdr)) {
        close(fd);
        (void)hv_stores(hv, "valid", newSViv(0));
        (void)hv_stores(hv, "reason", newSVpvs("short read on image header"));
        return hv;
    }
    if (S_shimg_validate_hdr(&hdr, (size_t)st.st_size, expected_manifest,
                             errbuf, sizeof(errbuf)) != 0) {
        close(fd);
        (void)hv_stores(hv, "valid", newSViv(0));
        (void)hv_stores(hv, "reason", newSVpv(errbuf, 0));
        return hv;
    }

    /* Verify payload checksum */
    if (hdr.payload_len > 0) {
        void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            close(fd);
            (void)hv_stores(hv, "valid", newSViv(0));
            (void)hv_stores(hv, "reason", newSVpvs("mmap failed during validation"));
            return hv;
        }
        uint64_t cksum = S_fnv1a_64((const uint8_t *)map + hdr.payload_offset,
                                    (size_t)hdr.payload_len);
        munmap(map, (size_t)st.st_size);
        if (cksum != hdr.payload_checksum) {
            close(fd);
            (void)hv_stores(hv, "valid", newSViv(0));
            (void)hv_stores(hv, "reason", newSVpvs("payload checksum mismatch"));
            return hv;
        }
    }
    close(fd);

    (void)hv_stores(hv, "valid",      newSViv(1));
    (void)hv_stores(hv, "reason",     newSVpvs("ok"));
    (void)hv_stores(hv, "fixed_addr", newSVuv((UV)hdr.fixed_addr));
    (void)hv_stores(hv, "build_id",   newSVpv(hdr.build_id, 0));
    (void)hv_stores(hv, "manifest",   newSVpv(hdr.manifest, 0));
    (void)hv_stores(hv, "payload_len", newSVuv((UV)hdr.payload_len));
    return hv;
}

HV *
Perl_mt_shimg_attach_hv(pTHX_ const char *path,
                        const char *expected_manifest,
                        UV force_fallback)
{
    int fd;
    struct stat st;
    perl_mt_shimg_hdr_t hdr;
    char errbuf[256];
    void *base;
    int exact = 0, fallback = 0;
    uint64_t cksum;
    uint32_t idx;
    perl_mt_shimg_info_t *info;
    HV *hv;

    if (!path || !*path)
        Perl_croak(aTHX_ "MT::SharedImage: path must be non-empty");

    fd = open(path, O_RDONLY);
    if (fd < 0)
        Perl_croak(aTHX_ "MT::SharedImage: open(%s) failed: %s", path, strerror(errno));

    if (fstat(fd, &st) < 0 || (size_t)st.st_size < sizeof(hdr)) {
        close(fd);
        Perl_croak(aTHX_ "MT::SharedImage: invalid image file %s", path);
    }
    if (pread(fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
        close(fd);
        Perl_croak(aTHX_ "MT::SharedImage: short header read on %s", path);
    }
    if (S_shimg_validate_hdr(&hdr, (size_t)st.st_size, expected_manifest,
                             errbuf, sizeof(errbuf)) != 0) {
        close(fd);
        Perl_croak(aTHX_ "MT::SharedImage: invalid image (%s)", errbuf);
    }

    base = S_shimg_mmap_try(fd, (size_t)hdr.file_size, PROT_READ, MAP_SHARED,
                            hdr.fixed_addr, force_fallback ? 1 : 0,
                            &exact, &fallback);
    if (base == MAP_FAILED) {
        int e = errno;
        close(fd);
        Perl_croak(aTHX_ "MT::SharedImage: mmap(%s) failed: %s", path, strerror(e));
    }

    cksum = S_fnv1a_64((const uint8_t *)base + hdr.payload_offset,
                       (size_t)hdr.payload_len);
    if (cksum != hdr.payload_checksum) {
        munmap(base, (size_t)hdr.file_size);
        close(fd);
        Perl_croak(aTHX_ "MT::SharedImage: payload checksum mismatch");
    }

    pthread_mutex_lock(&S_shimg_lock);
    if (S_shimg_alloc_slot(&idx) != 0) {
        pthread_mutex_unlock(&S_shimg_lock);
        munmap(base, (size_t)hdr.file_size);
        close(fd);
        Perl_croak(aTHX_ "MT::SharedImage: handle table full");
    }
    info = &S_shimg_slots[idx];
    info->fd               = fd;
    info->base             = base;
    info->map_len          = (size_t)hdr.file_size;
    info->requested_addr   = hdr.fixed_addr;
    info->mapped_addr      = (uint64_t)(uintptr_t)base;
    info->exact_addr_match = exact;
    info->fallback         = fallback;
    info->payload_offset   = hdr.payload_offset;
    info->payload_len      = hdr.payload_len;
    info->module_hash      = hdr.module_hash;
    info->payload_checksum = hdr.payload_checksum;
    S_copy_str(info->build_id, sizeof(info->build_id), hdr.build_id);
    S_copy_str(info->manifest, sizeof(info->manifest), hdr.manifest);
    S_shimg_arm_protection(info);

    hv = S_shimg_to_hv(aTHX_ info);
    pthread_mutex_unlock(&S_shimg_lock);
    return hv;
}

SV *
Perl_mt_shimg_payload_sv(pTHX_ UV id)
{
    perl_mt_shimg_info_t *info;
    SV *sv;

    pthread_mutex_lock(&S_shimg_lock);
    info = S_shimg_find_locked(id);
    if (!info || !info->base) {
        pthread_mutex_unlock(&S_shimg_lock);
        Perl_croak(aTHX_ "MT::SharedImage: invalid handle %" UVuf, id);
    }
    sv = newSVpvn((const char *)info->base + info->payload_offset,
                  (STRLEN)info->payload_len);
    pthread_mutex_unlock(&S_shimg_lock);
    return sv;
}

HV *
Perl_mt_shimg_info_hv(pTHX_ UV id)
{
    perl_mt_shimg_info_t *info;
    HV *hv;

    pthread_mutex_lock(&S_shimg_lock);
    info = S_shimg_find_locked(id);
    if (!info) {
        pthread_mutex_unlock(&S_shimg_lock);
        Perl_croak(aTHX_ "MT::SharedImage: invalid handle %" UVuf, id);
    }
    hv = S_shimg_to_hv(aTHX_ info);
    pthread_mutex_unlock(&S_shimg_lock);
    return hv;
}

int
Perl_mt_shimg_detach_id(UV id)
{
    perl_mt_shimg_info_t *info;
    pthread_mutex_lock(&S_shimg_lock);
    info = S_shimg_find_locked(id);
    if (!info) {
        pthread_mutex_unlock(&S_shimg_lock);
        return -ENOENT;
    }
    if (info->pkey != PERL_KRT_KEY_NONE) {
        perl_krt_key_free(info->pkey);
        info->pkey = PERL_KRT_KEY_NONE;
    }
    if (info->base && info->map_len > 0) {
        munmap(info->base, info->map_len);
        info->base = NULL;
    }
    if (info->fd >= 0) {
        close(info->fd);
        info->fd = -1;
    }
    info->active = 0;
    pthread_mutex_unlock(&S_shimg_lock);
    return 0;
}

/* =========================================================================
 * 10b: Worker-Group Namespaces (WPAR tier)
 * ========================================================================= */

typedef struct perl_mt_ns_entry {
    uint32_t id;
    int      active;
    char     name[MT_NS_NAME_MAX];
    char     cwd[MT_NS_PATH_MAX];
    uint32_t umask_val;
    uint64_t cap_mask;
    uint64_t enter_count;
} perl_mt_ns_entry_t;

static pthread_mutex_t    S_ns_lock = PTHREAD_MUTEX_INITIALIZER;
static perl_mt_ns_entry_t S_ns_slots[MT_NS_MAX];
static uint32_t           S_ns_next_id = 1;
static _Thread_local UV   S_tls_ns_id = 0;

HV *
Perl_mt_ns_caps_hv(pTHX)
{
    HV *hv = newHV();
#if defined(__linux__)
    (void)hv_stores(hv, "clone_fs",    newSViv(1));
    (void)hv_stores(hv, "clone_files", newSViv(1));
    (void)hv_stores(hv, "platform",    newSVpvs("linux"));
#elif defined(__APPLE__)
    (void)hv_stores(hv, "clone_fs",    newSViv(0));
    (void)hv_stores(hv, "clone_files", newSViv(0));
    (void)hv_stores(hv, "platform",    newSVpvs("darwin"));
#elif defined(_AIX)
    (void)hv_stores(hv, "clone_fs",    newSViv(0));
    (void)hv_stores(hv, "clone_files", newSViv(0));
    (void)hv_stores(hv, "platform",    newSVpvs("aix"));
#else
    (void)hv_stores(hv, "clone_fs",    newSViv(0));
    (void)hv_stores(hv, "clone_files", newSViv(0));
    (void)hv_stores(hv, "platform",    newSVpvs("portable"));
#endif
    (void)hv_stores(hv, "overlay",     newSViv(1));
    (void)hv_stores(hv, "fault_model", newSVpvs(MT_NS_FAULT_MODEL));
    return hv;
}

UV
Perl_mt_ns_create(const char *name, const char *cwd,
                  uint32_t umask_val, uint64_t cap_mask)
{
    uint32_t i;
    UV id = 0;

    pthread_mutex_lock(&S_ns_lock);
    for (i = 0; i < MT_NS_MAX; i++) {
        if (!S_ns_slots[i].active) {
            perl_mt_ns_entry_t *e = &S_ns_slots[i];
            memset(e, 0, sizeof(*e));
            e->active    = 1;
            e->id        = S_ns_next_id++;
            if (S_ns_next_id == 0) S_ns_next_id = 1;
            S_copy_str(e->name, sizeof(e->name), name ? name : "unnamed");
            if (cwd && *cwd) {
                S_copy_str(e->cwd, sizeof(e->cwd), cwd);
            } else {
                if (!getcwd(e->cwd, sizeof(e->cwd)))
                    S_copy_str(e->cwd, sizeof(e->cwd), "/");
            }
            e->umask_val = umask_val;
            e->cap_mask  = cap_mask;
            id = (UV)e->id;
            break;
        }
    }
    pthread_mutex_unlock(&S_ns_lock);
    return id;
}

int
Perl_mt_ns_destroy(UV id)
{
    uint32_t i;
    int rc = -ENOENT;
    pthread_mutex_lock(&S_ns_lock);
    for (i = 0; i < MT_NS_MAX; i++) {
        if (S_ns_slots[i].active && (UV)S_ns_slots[i].id == id) {
            S_ns_slots[i].active = 0;
            rc = 0;
            break;
        }
    }
    pthread_mutex_unlock(&S_ns_lock);
    return rc;
}

int
Perl_mt_ns_enter(UV id, int isolate_os)
{
    uint32_t i;
    int found = 0;
    char cwd[MT_NS_PATH_MAX];
    uint32_t um = 022;

    pthread_mutex_lock(&S_ns_lock);
    for (i = 0; i < MT_NS_MAX; i++) {
        if (S_ns_slots[i].active && (UV)S_ns_slots[i].id == id) {
            S_ns_slots[i].enter_count++;
            S_copy_str(cwd, sizeof(cwd), S_ns_slots[i].cwd);
            um = S_ns_slots[i].umask_val;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&S_ns_lock);
    if (!found) return -ENOENT;

    S_tls_ns_id = id;
    if (isolate_os) {
#if defined(__linux__) && defined(CLONE_FS) && defined(CLONE_FILES)
        if (unshare(CLONE_FS | CLONE_FILES) == 0) {
            (void)chdir(cwd);
            (void)umask((mode_t)um);
            return 0;
        }
        return -errno;
#else
        (void)cwd;
        (void)um;
        return PERL_KRT_ENOSYS;
#endif
    }
    return 0;
}

void
Perl_mt_ns_leave(void)
{
    S_tls_ns_id = 0;
}

UV
Perl_mt_ns_current(void)
{
    return S_tls_ns_id;
}

HV *
Perl_mt_ns_info_hv(pTHX_ UV id)
{
    uint32_t i;
    HV *hv = NULL;

    pthread_mutex_lock(&S_ns_lock);
    for (i = 0; i < MT_NS_MAX; i++) {
        if (S_ns_slots[i].active && (UV)S_ns_slots[i].id == id) {
            perl_mt_ns_entry_t *e = &S_ns_slots[i];
            hv = newHV();
            (void)hv_stores(hv, "id",          newSVuv((UV)e->id));
            (void)hv_stores(hv, "name",        newSVpv(e->name, 0));
            (void)hv_stores(hv, "cwd",         newSVpv(e->cwd, 0));
            (void)hv_stores(hv, "umask",       newSVuv((UV)e->umask_val));
            (void)hv_stores(hv, "cap_mask",    newSVuv((UV)e->cap_mask));
            (void)hv_stores(hv, "enter_count", newSVuv((UV)e->enter_count));
            (void)hv_stores(hv, "fault_model", newSVpvs(MT_NS_FAULT_MODEL));
            break;
        }
    }
    pthread_mutex_unlock(&S_ns_lock);
    if (!hv)
        Perl_croak(aTHX_ "MT::Namespace: unknown namespace id %" UVuf, id);
    return hv;
}

/* =========================================================================
 * 10c: Resource Classes (WLM tier)
 * ========================================================================= */

typedef struct perl_mt_class_entry {
    uint32_t        id;
    int             active;
    char            name[MT_CLASS_NAME_MAX];
    uint32_t        cpu_weight;
    uint64_t        mem_limit_bytes;
    uint64_t        io_budget_ops;
    uint64_t        io_window_ms;
    perl_krt_au64_t mem_used_bytes;
    perl_krt_au64_t mem_peak_bytes;
    perl_krt_au64_t mem_denied;
    perl_krt_au64_t cpu_units_used;
    perl_krt_au64_t cpu_throttled;
    perl_krt_au64_t io_ops_total;
    perl_krt_au64_t io_ops_window;
    perl_krt_au64_t io_window_start_ns;
    perl_krt_au64_t io_denied;
} perl_mt_class_entry_t;

static pthread_mutex_t       S_class_lock = PTHREAD_MUTEX_INITIALIZER;
static perl_mt_class_entry_t S_class_slots[MT_CLASS_MAX];
static uint32_t              S_class_next_id = 1;

static perl_mt_class_entry_t *
S_class_find(UV id)
{
    uint32_t i;
    for (i = 0; i < MT_CLASS_MAX; i++) {
        if (S_class_slots[i].active && (UV)S_class_slots[i].id == id)
            return &S_class_slots[i];
    }
    return NULL;
}

UV
Perl_mt_class_create(const char *name, uint32_t cpu_weight,
                     uint64_t mem_limit_bytes, uint64_t io_budget_ops,
                     uint64_t io_window_ms)
{
    uint32_t i;
    UV id = 0;

    if (cpu_weight == 0) cpu_weight = 100;
    if (cpu_weight > 10000) cpu_weight = 10000;
    if (io_window_ms == 0) io_window_ms = 1000;

    pthread_mutex_lock(&S_class_lock);
    for (i = 0; i < MT_CLASS_MAX; i++) {
        if (!S_class_slots[i].active) {
            perl_mt_class_entry_t *c = &S_class_slots[i];
            memset(c, 0, sizeof(*c));
            c->active          = 1;
            c->id              = S_class_next_id++;
            if (S_class_next_id == 0) S_class_next_id = 1;
            S_copy_str(c->name, sizeof(c->name), name ? name : "default");
            c->cpu_weight      = cpu_weight;
            c->mem_limit_bytes = mem_limit_bytes;
            c->io_budget_ops   = io_budget_ops;
            c->io_window_ms    = io_window_ms;
            perl_krt_store_relaxed(&c->io_window_start_ns, perl_krt_now_ns());
            id = (UV)c->id;
            break;
        }
    }
    pthread_mutex_unlock(&S_class_lock);
    return id;
}

int
Perl_mt_class_destroy(UV id)
{
    int rc = -ENOENT;
    pthread_mutex_lock(&S_class_lock);
    perl_mt_class_entry_t *c = S_class_find(id);
    if (c) {
        c->active = 0;
        rc = 0;
    }
    pthread_mutex_unlock(&S_class_lock);
    return rc;
}

int
Perl_mt_class_bind_thread(UV id)
{
    uint32_t weight = 100;
    pthread_mutex_lock(&S_class_lock);
    perl_mt_class_entry_t *c = S_class_find(id);
    if (!c) {
        pthread_mutex_unlock(&S_class_lock);
        return -ENOENT;
    }
    weight = c->cpu_weight;
    pthread_mutex_unlock(&S_class_lock);

#if defined(__APPLE__)
    {
        qos_class_t qos = QOS_CLASS_DEFAULT;
        if (weight >= 75)      qos = QOS_CLASS_USER_INITIATED;
        else if (weight >= 40) qos = QOS_CLASS_DEFAULT;
        else if (weight >= 15) qos = QOS_CLASS_UTILITY;
        else                   qos = QOS_CLASS_BACKGROUND;
        (void)pthread_set_qos_class_self_np(qos, 0);
    }
#endif
    (void)weight;
    return 0;
}

int
Perl_mt_class_charge_mem(UV id, uint64_t bytes)
{
    perl_mt_class_entry_t *c;
    uint64_t cur, next, peak;

    pthread_mutex_lock(&S_class_lock);
    c = S_class_find(id);
    if (!c) {
        pthread_mutex_unlock(&S_class_lock);
        return -ENOENT;
    }
    cur = perl_krt_load_relaxed(&c->mem_used_bytes);
    next = cur + bytes;
    if (c->mem_limit_bytes > 0 && next > c->mem_limit_bytes) {
        perl_krt_fetch_add_relaxed(&c->mem_denied, 1);
        pthread_mutex_unlock(&S_class_lock);
        return -ENOMEM;
    }
    perl_krt_store_relaxed(&c->mem_used_bytes, next);
    peak = perl_krt_load_relaxed(&c->mem_peak_bytes);
    if (next > peak)
        perl_krt_store_relaxed(&c->mem_peak_bytes, next);
    pthread_mutex_unlock(&S_class_lock);
    return 0;
}

int
Perl_mt_class_release_mem(UV id, uint64_t bytes)
{
    perl_mt_class_entry_t *c;
    uint64_t cur;

    pthread_mutex_lock(&S_class_lock);
    c = S_class_find(id);
    if (!c) {
        pthread_mutex_unlock(&S_class_lock);
        return -ENOENT;
    }
    cur = perl_krt_load_relaxed(&c->mem_used_bytes);
    perl_krt_store_relaxed(&c->mem_used_bytes, (bytes >= cur) ? 0 : (cur - bytes));
    pthread_mutex_unlock(&S_class_lock);
    return 0;
}

int
Perl_mt_class_charge_io(UV id, uint64_t ops)
{
    perl_mt_class_entry_t *c;
    uint64_t now, start, win_ns, cur;

    pthread_mutex_lock(&S_class_lock);
    c = S_class_find(id);
    if (!c) {
        pthread_mutex_unlock(&S_class_lock);
        return -ENOENT;
    }
    now = perl_krt_now_ns();
    start = perl_krt_load_relaxed(&c->io_window_start_ns);
    win_ns = c->io_window_ms * 1000000ULL;
    if (now >= start && (now - start) >= win_ns) {
        perl_krt_store_relaxed(&c->io_window_start_ns, now);
        perl_krt_store_relaxed(&c->io_ops_window, 0);
    }
    cur = perl_krt_load_relaxed(&c->io_ops_window);
    if (c->io_budget_ops > 0 && cur + ops > c->io_budget_ops) {
        perl_krt_fetch_add_relaxed(&c->io_denied, 1);
        pthread_mutex_unlock(&S_class_lock);
        return -EAGAIN;
    }
    perl_krt_store_relaxed(&c->io_ops_window, cur + ops);
    perl_krt_fetch_add_relaxed(&c->io_ops_total, ops);
    pthread_mutex_unlock(&S_class_lock);
    return 0;
}

int
Perl_mt_class_charge_cpu(UV id, uint64_t work_units)
{
    perl_mt_class_entry_t *c;
    uint32_t weight = 100;
    uint64_t used;
    uint32_t max_weight = 0;
    uint32_t i;

    pthread_mutex_lock(&S_class_lock);
    c = S_class_find(id);
    if (!c) {
        pthread_mutex_unlock(&S_class_lock);
        return -ENOENT;
    }
    weight = c->cpu_weight;
    used = perl_krt_fetch_add_relaxed(&c->cpu_units_used, work_units) + work_units;
    for (i = 0; i < MT_CLASS_MAX; i++) {
        if (S_class_slots[i].active && S_class_slots[i].cpu_weight > max_weight)
            max_weight = S_class_slots[i].cpu_weight;
    }
    pthread_mutex_unlock(&S_class_lock);

    /* Proportional pacing when a lower-weight class is competing with a
     * higher-weight class so runaway groups cannot starve higher-priority work. */
    if (max_weight > weight && weight > 0) {
        uint32_t ratio = max_weight / weight;
        if (ratio >= 2 && (used % 4) == 0) {
            struct timespec ts;
            uint64_t us = (uint64_t)ratio * 50ULL;
            if (us > 2000) us = 2000;
            ts.tv_sec  = 0;
            ts.tv_nsec = (long)(us * 1000ULL);
            nanosleep(&ts, NULL);
            perl_krt_fetch_add_relaxed(&c->cpu_throttled, 1);
        }
    }
    return 0;
}

HV *
Perl_mt_class_stats_hv(pTHX_ UV id)
{
    perl_mt_class_entry_t *c;
    HV *hv = NULL;
    const char *cpu_backend = "advisory";

#if defined(__linux__)
    if (access("/sys/fs/cgroup/cgroup.controllers", F_OK) == 0)
        cpu_backend = "cgroup_v2";
    else
        cpu_backend = "linux_advisory";
#elif defined(__APPLE__)
    cpu_backend = "darwin_qos_advisory";
#endif

    pthread_mutex_lock(&S_class_lock);
    c = S_class_find(id);
    if (c) {
        hv = newHV();
        (void)hv_stores(hv, "id",              newSVuv((UV)c->id));
        (void)hv_stores(hv, "name",            newSVpv(c->name, 0));
        (void)hv_stores(hv, "cpu_weight",      newSVuv((UV)c->cpu_weight));
        (void)hv_stores(hv, "cpu_backend",     newSVpv(cpu_backend, 0));
        (void)hv_stores(hv, "mem_limit_bytes", newSVuv((UV)c->mem_limit_bytes));
        (void)hv_stores(hv, "mem_used_bytes",  newSVuv((UV)perl_krt_load_relaxed(&c->mem_used_bytes)));
        (void)hv_stores(hv, "mem_peak_bytes",  newSVuv((UV)perl_krt_load_relaxed(&c->mem_peak_bytes)));
        (void)hv_stores(hv, "mem_denied",      newSVuv((UV)perl_krt_load_relaxed(&c->mem_denied)));
        (void)hv_stores(hv, "io_budget_ops",   newSVuv((UV)c->io_budget_ops));
        (void)hv_stores(hv, "io_window_ms",    newSVuv((UV)c->io_window_ms));
        (void)hv_stores(hv, "io_ops_total",    newSVuv((UV)perl_krt_load_relaxed(&c->io_ops_total)));
        (void)hv_stores(hv, "io_ops_window",   newSVuv((UV)perl_krt_load_relaxed(&c->io_ops_window)));
        (void)hv_stores(hv, "io_denied",       newSVuv((UV)perl_krt_load_relaxed(&c->io_denied)));
        (void)hv_stores(hv, "cpu_units_used",  newSVuv((UV)perl_krt_load_relaxed(&c->cpu_units_used)));
        (void)hv_stores(hv, "cpu_throttled",   newSVuv((UV)perl_krt_load_relaxed(&c->cpu_throttled)));
    }
    pthread_mutex_unlock(&S_class_lock);
    if (!hv)
        Perl_croak(aTHX_ "MT::Class: unknown class id %" UVuf, id);
    return hv;
}

/* =========================================================================
 * 10e: Cross-Process Pairs & Supervisor Pair
 * ========================================================================= */

typedef struct perl_mt_pair_handle {
    uint32_t            id;
    int                 active;
    int                 fd;
    size_t              map_len;
    perl_mt_pair_shm_t *shm;
} perl_mt_pair_handle_t;

static pthread_mutex_t       S_pair_lock = PTHREAD_MUTEX_INITIALIZER;
static perl_mt_pair_handle_t S_pair_slots[MT_PAIR_MAX_HANDLES];
static uint32_t              S_pair_next_id = 1;

static perl_mt_pair_handle_t *
S_pair_find_locked(UV id)
{
    uint32_t i;
    for (i = 0; i < MT_PAIR_MAX_HANDLES; i++) {
        if (S_pair_slots[i].active && (UV)S_pair_slots[i].id == id)
            return &S_pair_slots[i];
    }
    return NULL;
}

UV
Perl_mt_pair_open(const char *path, const char *name,
                  const char *image_path, uint32_t hb_timeout_ms,
                  int is_supervisor, int create)
{
    size_t map_len = S_round_up_page(sizeof(perl_mt_pair_shm_t));
    int flags = create ? (O_RDWR | O_CREAT) : O_RDWR;
    int fd;
    void *base;
    perl_mt_pair_shm_t *shm;
    uint32_t i;
    UV id = 0;

    if (!path || !*path)
        return 0;

    fd = open(path, flags, 0644);
    if (fd < 0)
        return 0;

    if (create) {
        if (ftruncate(fd, (off_t)map_len) < 0) {
            close(fd);
            return 0;
        }
    }

    base = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return 0;
    }
    shm = (perl_mt_pair_shm_t *)base;

    if (create) {
        memset(shm, 0, sizeof(*shm));
        shm->magic         = MT_PAIR_MAGIC;
        shm->version       = MT_PAIR_VERSION;
        shm->hb_timeout_ms = hb_timeout_ms ? hb_timeout_ms : 300u;
        shm->is_supervisor = is_supervisor ? 1u : 0u;
        S_copy_str(shm->name, sizeof(shm->name), name ? name : "pair");
        S_copy_str(shm->image_path, sizeof(shm->image_path),
                   image_path ? image_path : "");
        perl_krt_store_release(&shm->active_role, MT_PAIR_ROLE_PRIMARY);
        perl_krt_store_release(&shm->epoch, 1u);
        perl_krt_store_release(&shm->state, MT_PAIR_STATE_INIT);
    } else {
        if (shm->magic != MT_PAIR_MAGIC || shm->version != MT_PAIR_VERSION) {
            munmap(base, map_len);
            close(fd);
            return 0;
        }
    }

    pthread_mutex_lock(&S_pair_lock);
    for (i = 0; i < MT_PAIR_MAX_HANDLES; i++) {
        if (!S_pair_slots[i].active) {
            S_pair_slots[i].active  = 1;
            S_pair_slots[i].id      = S_pair_next_id++;
            if (S_pair_next_id == 0) S_pair_next_id = 1;
            S_pair_slots[i].fd      = fd;
            S_pair_slots[i].map_len = map_len;
            S_pair_slots[i].shm     = shm;
            id = (UV)S_pair_slots[i].id;
            break;
        }
    }
    pthread_mutex_unlock(&S_pair_lock);

    if (id == 0) {
        munmap(base, map_len);
        close(fd);
    }
    return id;
}

int
Perl_mt_pair_close(UV id)
{
    perl_mt_pair_handle_t *h;
    pthread_mutex_lock(&S_pair_lock);
    h = S_pair_find_locked(id);
    if (!h) {
        pthread_mutex_unlock(&S_pair_lock);
        return -ENOENT;
    }
    if (h->shm && h->map_len > 0)
        munmap(h->shm, h->map_len);
    if (h->fd >= 0)
        close(h->fd);
    h->shm = NULL;
    h->fd = -1;
    h->active = 0;
    pthread_mutex_unlock(&S_pair_lock);
    return 0;
}

int
Perl_mt_pair_register(UV id, uint32_t role)
{
    perl_mt_pair_handle_t *h;
    perl_mt_pair_shm_t *shm;
    uint64_t now = perl_krt_now_ns();
    uint32_t pid = (uint32_t)getpid();

    pthread_mutex_lock(&S_pair_lock);
    h = S_pair_find_locked(id);
    shm = h ? h->shm : NULL;
    pthread_mutex_unlock(&S_pair_lock);
    if (!shm) return -ENOENT;

    if (role == MT_PAIR_ROLE_PRIMARY) {
        perl_krt_store_release(&shm->primary_pid, pid);
        perl_krt_store_release(&shm->primary_hb_ns, now);
        perl_krt_store_release(&shm->active_role, MT_PAIR_ROLE_PRIMARY);
        perl_krt_store_release(&shm->state, MT_PAIR_STATE_ACTIVE);
    } else if (role == MT_PAIR_ROLE_BACKUP) {
        perl_krt_store_release(&shm->backup_pid, pid);
        perl_krt_store_release(&shm->backup_hb_ns, now);
    } else {
        return -EINVAL;
    }
    return 0;
}

int
Perl_mt_pair_heartbeat(UV id, uint32_t role)
{
    perl_mt_pair_handle_t *h;
    perl_mt_pair_shm_t *shm;
    uint64_t now = perl_krt_now_ns();

    pthread_mutex_lock(&S_pair_lock);
    h = S_pair_find_locked(id);
    shm = h ? h->shm : NULL;
    pthread_mutex_unlock(&S_pair_lock);
    if (!shm) return -ENOENT;

    if (role == MT_PAIR_ROLE_PRIMARY)
        perl_krt_store_release(&shm->primary_hb_ns, now);
    else if (role == MT_PAIR_ROLE_BACKUP)
        perl_krt_store_release(&shm->backup_hb_ns, now);
    else
        return -EINVAL;
    return 0;
}

uint64_t
Perl_mt_pair_checkpoint(UV id, const void *data, size_t len)
{
    perl_mt_pair_handle_t *h;
    perl_mt_pair_shm_t *shm;
    uint64_t seq;
    uint32_t slot_idx;
    perl_mt_pair_ckpt_t *slot;

    if (len > MT_PAIR_CKPT_DATA_MAX)
        return 0;

    pthread_mutex_lock(&S_pair_lock);
    h = S_pair_find_locked(id);
    shm = h ? h->shm : NULL;
    pthread_mutex_unlock(&S_pair_lock);
    if (!shm) return 0;

    seq = perl_krt_fetch_add_acq_rel(&shm->checkpoint_seq, 1ULL) + 1ULL;
    slot_idx = (uint32_t)((seq - 1ULL) % MT_PAIR_RING_SLOTS);
    slot = &shm->ring[slot_idx];

    /* Mark in-progress (seq=0), copy payload + checksum, then publish seq. */
    perl_krt_store_release(&slot->seq, 0ULL);
    slot->timestamp_ns = perl_krt_now_ns();
    slot->data_len     = (uint32_t)len;
    if (len > 0 && data)
        memcpy(slot->data, data, len);
    slot->checksum     = S_fnv1a_32(slot->data, len);
    perl_krt_store_release(&slot->seq, seq);
    perl_krt_store_release(&shm->ring_head, slot_idx);
    perl_krt_store_release(&shm->primary_hb_ns, slot->timestamp_ns);
    return seq;
}

HV *
Perl_mt_pair_last_checkpoint_hv(pTHX_ UV id)
{
    perl_mt_pair_handle_t *h;
    perl_mt_pair_shm_t *shm;
    uint64_t best_seq = 0;
    int best_idx = -1;
    uint32_t i;
    HV *hv = newHV();

    pthread_mutex_lock(&S_pair_lock);
    h = S_pair_find_locked(id);
    shm = h ? h->shm : NULL;
    pthread_mutex_unlock(&S_pair_lock);
    if (!shm)
        Perl_croak(aTHX_ "MT::ProcessPair: invalid handle %" UVuf, id);

    for (i = 0; i < MT_PAIR_RING_SLOTS; i++) {
        uint64_t s = perl_krt_load_acquire(&shm->ring[i].seq);
        if (s > best_seq) {
            uint32_t dlen = shm->ring[i].data_len;
            if (dlen <= MT_PAIR_CKPT_DATA_MAX &&
                S_fnv1a_32(shm->ring[i].data, dlen) == shm->ring[i].checksum) {
                best_seq = s;
                best_idx = (int)i;
            }
        }
    }

    if (best_idx < 0) {
        (void)hv_stores(hv, "found", newSViv(0));
        (void)hv_stores(hv, "seq",   newSVuv(0));
        return hv;
    }

    (void)hv_stores(hv, "found",        newSViv(1));
    (void)hv_stores(hv, "seq",          newSVuv((UV)best_seq));
    (void)hv_stores(hv, "timestamp_ns", newSVuv((UV)shm->ring[best_idx].timestamp_ns));
    (void)hv_stores(hv, "checksum",     newSVuv((UV)shm->ring[best_idx].checksum));
    (void)hv_stores(hv, "data",
                    newSVpvn((const char *)shm->ring[best_idx].data,
                             (STRLEN)shm->ring[best_idx].data_len));
    return hv;
}

int
Perl_mt_pair_check_failover(UV id)
{
    perl_mt_pair_handle_t *h;
    perl_mt_pair_shm_t *shm;
    uint32_t role, ppid;
    uint64_t last_hb, now, timeout_ns;
    int primary_dead = 0;

    pthread_mutex_lock(&S_pair_lock);
    h = S_pair_find_locked(id);
    shm = h ? h->shm : NULL;
    pthread_mutex_unlock(&S_pair_lock);
    if (!shm) return -ENOENT;

    role = perl_krt_load_acquire(&shm->active_role);
    if (role != MT_PAIR_ROLE_PRIMARY)
        return 0; /* already failed over */

    ppid = perl_krt_load_acquire(&shm->primary_pid);
    if (ppid > 0 && ppid != (uint32_t)getpid()) {
        if (kill((pid_t)ppid, 0) < 0 && errno == ESRCH)
            primary_dead = 1;
    }

    last_hb = perl_krt_load_acquire(&shm->primary_hb_ns);
    now = perl_krt_now_ns();
    timeout_ns = (uint64_t)shm->hb_timeout_ms * 1000000ULL;
    if (!primary_dead && last_hb > 0 && now > last_hb &&
        (now - last_hb) > timeout_ns) {
        primary_dead = 1;
    }

    if (!primary_dead)
        return 0;

    /* Atomically promote backup -> active primary role */
    uint32_t expected = MT_PAIR_ROLE_PRIMARY;
    if (perl_krt_cas_strong_acq_rel(&shm->active_role, &expected, MT_PAIR_ROLE_BACKUP)) {
        perl_krt_fetch_add_acq_rel(&shm->epoch, 1u);
        perl_krt_store_release(&shm->primary_pid, (uint32_t)getpid());
        perl_krt_store_release(&shm->primary_hb_ns, now);
        perl_krt_store_release(&shm->state, MT_PAIR_STATE_FAILOVER);
        return 1;
    }
    return 0;
}

HV *
Perl_mt_pair_status_hv(pTHX_ UV id)
{
    perl_mt_pair_handle_t *h;
    perl_mt_pair_shm_t *shm;
    HV *hv = newHV();

    pthread_mutex_lock(&S_pair_lock);
    h = S_pair_find_locked(id);
    shm = h ? h->shm : NULL;
    pthread_mutex_unlock(&S_pair_lock);
    if (!shm)
        Perl_croak(aTHX_ "MT::ProcessPair: invalid handle %" UVuf, id);

    (void)hv_stores(hv, "name",           newSVpv(shm->name, 0));
    (void)hv_stores(hv, "image_path",     newSVpv(shm->image_path, 0));
    (void)hv_stores(hv, "is_supervisor",  newSViv(shm->is_supervisor ? 1 : 0));
    (void)hv_stores(hv, "primary_pid",    newSVuv((UV)perl_krt_load_acquire(&shm->primary_pid)));
    (void)hv_stores(hv, "backup_pid",     newSVuv((UV)perl_krt_load_acquire(&shm->backup_pid)));
    (void)hv_stores(hv, "active_role",    newSVuv((UV)perl_krt_load_acquire(&shm->active_role)));
    (void)hv_stores(hv, "epoch",          newSVuv((UV)perl_krt_load_acquire(&shm->epoch)));
    (void)hv_stores(hv, "state",          newSVuv((UV)perl_krt_load_acquire(&shm->state)));
    (void)hv_stores(hv, "hb_timeout_ms",  newSVuv((UV)shm->hb_timeout_ms));
    (void)hv_stores(hv, "checkpoint_seq", newSVuv((UV)perl_krt_load_acquire(&shm->checkpoint_seq)));
    (void)hv_stores(hv, "tx_epoch",       newSVuv((UV)perl_krt_load_acquire(&shm->tx_epoch)));
    return hv;
}

static void
S_pair_tx_lock(perl_mt_pair_shm_t *shm)
{
    uint32_t expected = 0;
    while (!perl_krt_cas_strong_acq_rel(&shm->tx_lock, &expected, 1u)) {
        expected = 0;
        perl_krt_cpu_relax();
    }
}

static void
S_pair_tx_unlock(perl_mt_pair_shm_t *shm)
{
    perl_krt_store_release(&shm->tx_lock, 0u);
}

int
Perl_mt_pair_tx_commit(pTHX_ UV id, HV *kv_hv)
{
    perl_mt_pair_handle_t *h;
    perl_mt_pair_shm_t *shm;
    HE *he;
    uint32_t needed = 0, existing_free = 0, i;

    pthread_mutex_lock(&S_pair_lock);
    h = S_pair_find_locked(id);
    shm = h ? h->shm : NULL;
    pthread_mutex_unlock(&S_pair_lock);
    if (!shm || !kv_hv) return -EINVAL;

    S_pair_tx_lock(shm);

    /* Step 1: validate all keys/values fit before mutating anything (atomic all-or-nothing) */
    for (i = 0; i < MT_PAIR_KV_MAX; i++) {
        if (!shm->kv[i].used) existing_free++;
    }
    hv_iterinit(kv_hv);
    while ((he = hv_iternext(kv_hv)) != NULL) {
        I32 klen = 0;
        const char *k = hv_iterkey(he, &klen);
        SV *vsv = hv_iterval(kv_hv, he);
        STRLEN vlen = 0;
        (void)SvPV_const(vsv, vlen);
        if (klen <= 0 || (size_t)klen >= MT_PAIR_KEY_MAX ||
            (size_t)vlen >= MT_PAIR_VAL_MAX) {
            S_pair_tx_unlock(shm);
            return -EINVAL;
        }
        int found = 0;
        for (i = 0; i < MT_PAIR_KV_MAX; i++) {
            if (shm->kv[i].used && strncmp(shm->kv[i].key, k, MT_PAIR_KEY_MAX) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) needed++;
    }
    if (needed > existing_free) {
        S_pair_tx_unlock(shm);
        return -ENOSPC;
    }

    /* Step 2: apply all writes and bump tx_epoch */
    uint64_t next_epoch = perl_krt_load_relaxed(&shm->tx_epoch) + 1ULL;
    hv_iterinit(kv_hv);
    while ((he = hv_iternext(kv_hv)) != NULL) {
        I32 klen = 0;
        const char *k = hv_iterkey(he, &klen);
        SV *vsv = hv_iterval(kv_hv, he);
        STRLEN vlen = 0;
        const char *v = SvPV_const(vsv, vlen);
        int slot = -1;
        for (i = 0; i < MT_PAIR_KV_MAX; i++) {
            if (shm->kv[i].used && strncmp(shm->kv[i].key, k, MT_PAIR_KEY_MAX) == 0) {
                slot = (int)i;
                break;
            }
        }
        if (slot < 0) {
            for (i = 0; i < MT_PAIR_KV_MAX; i++) {
                if (!shm->kv[i].used) {
                    slot = (int)i;
                    shm->kv[i].used = 1;
                    shm->kv_count++;
                    break;
                }
            }
        }
        if (slot >= 0) {
            S_copy_str(shm->kv[slot].key, MT_PAIR_KEY_MAX, k);
            S_copy_str(shm->kv[slot].val, MT_PAIR_VAL_MAX, v);
            shm->kv[slot].version = next_epoch;
        }
    }
    perl_krt_store_release(&shm->tx_epoch, next_epoch);
    S_pair_tx_unlock(shm);
    return 0;
}

HV *
Perl_mt_pair_tx_snapshot_hv(pTHX_ UV id)
{
    perl_mt_pair_handle_t *h;
    perl_mt_pair_shm_t *shm;
    HV *hv = newHV();
    uint32_t i;

    pthread_mutex_lock(&S_pair_lock);
    h = S_pair_find_locked(id);
    shm = h ? h->shm : NULL;
    pthread_mutex_unlock(&S_pair_lock);
    if (!shm)
        Perl_croak(aTHX_ "MT::ProcessPair: invalid handle %" UVuf, id);

    S_pair_tx_lock(shm);
    for (i = 0; i < MT_PAIR_KV_MAX; i++) {
        if (shm->kv[i].used) {
            (void)hv_store(hv, shm->kv[i].key, (I32)strlen(shm->kv[i].key),
                           newSVpv(shm->kv[i].val, 0), 0);
        }
    }
    S_pair_tx_unlock(shm);
    return hv;
}

/* =========================================================================
 * 10f: Threadsafe SHM-backed NDArray (PDL Bridge)
 * ========================================================================= */

typedef struct perl_mt_ndarray {
    uint32_t       id;
    uint32_t       gen;
    int            active;
    int            sealed;
    uint32_t       dtype;
    uint32_t       ndims;
    uint64_t       dims[MT_NDARRAY_MAX_DIMS];
    uint64_t       nelem;
    perl_krt_shm_t shm;
} perl_mt_ndarray_t;

static pthread_mutex_t   S_nd_lock = PTHREAD_MUTEX_INITIALIZER;
static perl_mt_ndarray_t S_nd_slots[MT_NDARRAY_MAX];
static uint32_t          S_nd_next_id = 1;

static perl_mt_ndarray_t *
S_nd_find(UV id)
{
    uint32_t i;
    for (i = 0; i < MT_NDARRAY_MAX; i++) {
        if (S_nd_slots[i].active && (UV)S_nd_slots[i].id == id)
            return &S_nd_slots[i];
    }
    return NULL;
}

UV
Perl_mt_ndarray_create(uint32_t dtype, UV nelem,
                       const uint64_t *dims, uint32_t ndims)
{
    size_t elem_sz = (dtype == MT_NDARRAY_DTYPE_I64) ? sizeof(int64_t) : sizeof(double);
    size_t bytes;
    perl_krt_shm_t shm;
    uint32_t i;
    UV id = 0;

    if (nelem == 0) nelem = 1;
    bytes = S_round_up_page((size_t)nelem * elem_sz);
    memset(&shm, 0, sizeof(shm));
    if (perl_krt_shm_create(&shm, bytes, 0) != 0)
        return 0;
    memset(shm.base, 0, bytes);

    pthread_mutex_lock(&S_nd_lock);
    for (i = 0; i < MT_NDARRAY_MAX; i++) {
        if (!S_nd_slots[i].active) {
            perl_mt_ndarray_t *nd = &S_nd_slots[i];
            uint32_t d;
            nd->gen++;
            nd->active = 1;
            nd->sealed = 0;
            nd->id     = S_nd_next_id++;
            if (S_nd_next_id == 0) S_nd_next_id = 1;
            nd->dtype  = (dtype == MT_NDARRAY_DTYPE_I64) ? MT_NDARRAY_DTYPE_I64 : MT_NDARRAY_DTYPE_F64;
            nd->nelem  = (uint64_t)nelem;
            nd->ndims  = (ndims > MT_NDARRAY_MAX_DIMS) ? MT_NDARRAY_MAX_DIMS : ndims;
            if (nd->ndims == 0) {
                nd->ndims = 1;
                nd->dims[0] = (uint64_t)nelem;
            } else {
                for (d = 0; d < nd->ndims; d++)
                    nd->dims[d] = dims ? dims[d] : (uint64_t)nelem;
            }
            nd->shm = shm;
            id = (UV)nd->id;
            break;
        }
    }
    pthread_mutex_unlock(&S_nd_lock);

    if (id == 0)
        perl_krt_shm_destroy(&shm);
    return id;
}

int
Perl_mt_ndarray_destroy(UV id)
{
    perl_mt_ndarray_t *nd;
    perl_krt_shm_t shm;
    int found = 0;

    pthread_mutex_lock(&S_nd_lock);
    nd = S_nd_find(id);
    if (nd) {
        shm = nd->shm;
        nd->active = 0;
        nd->sealed = 0;
        found = 1;
    }
    pthread_mutex_unlock(&S_nd_lock);
    if (!found) return -ENOENT;
    perl_krt_shm_destroy(&shm);
    return 0;
}

int
Perl_mt_ndarray_seal(UV id)
{
    perl_mt_ndarray_t *nd;
    int rc = -ENOENT;

    pthread_mutex_lock(&S_nd_lock);
    nd = S_nd_find(id);
    if (nd) {
        if (!nd->sealed) {
            rc = perl_krt_shm_seal_ro(&nd->shm, nd->shm.base, nd->shm.len);
            if (rc == 0) nd->sealed = 1;
        } else {
            rc = 0;
        }
    }
    pthread_mutex_unlock(&S_nd_lock);
    return rc;
}

int
Perl_mt_ndarray_is_sealed(UV id)
{
    perl_mt_ndarray_t *nd;
    int s = 0;
    pthread_mutex_lock(&S_nd_lock);
    nd = S_nd_find(id);
    if (nd) s = nd->sealed;
    pthread_mutex_unlock(&S_nd_lock);
    return s;
}

int
Perl_mt_ndarray_set_f64(UV id, UV idx, double val)
{
    perl_mt_ndarray_t *nd;
    double *p = NULL;

    pthread_mutex_lock(&S_nd_lock);
    nd = S_nd_find(id);
    if (!nd) {
        pthread_mutex_unlock(&S_nd_lock);
        return -ENOENT;
    }
    if (nd->sealed) {
        pthread_mutex_unlock(&S_nd_lock);
        return -EACCES;
    }
    if ((uint64_t)idx >= nd->nelem) {
        pthread_mutex_unlock(&S_nd_lock);
        return -ERANGE;
    }
    p = (double *)nd->shm.base;
    p[idx] = val;
    pthread_mutex_unlock(&S_nd_lock);
    return 0;
}

double
Perl_mt_ndarray_get_f64(UV id, UV idx)
{
    perl_mt_ndarray_t *nd;
    double v = 0.0;

    pthread_mutex_lock(&S_nd_lock);
    nd = S_nd_find(id);
    if (nd && (uint64_t)idx < nd->nelem)
        v = ((const double *)nd->shm.base)[idx];
    pthread_mutex_unlock(&S_nd_lock);
    return v;
}

int
Perl_mt_ndarray_fill_seq(UV id, double start, double step)
{
    perl_mt_ndarray_t *nd;
    double *p;
    uint64_t n, i;

    pthread_mutex_lock(&S_nd_lock);
    nd = S_nd_find(id);
    if (!nd) {
        pthread_mutex_unlock(&S_nd_lock);
        return -ENOENT;
    }
    if (nd->sealed) {
        pthread_mutex_unlock(&S_nd_lock);
        return -EACCES;
    }
    p = (double *)nd->shm.base;
    n = nd->nelem;
    pthread_mutex_unlock(&S_nd_lock);

    for (i = 0; i < n; i++)
        p[i] = start + (double)i * step;
    return 0;
}

/* Deterministic slice kernel: dst[i] = ((src[i] * alpha) + beta) * (src[i] + 0.5).
 * No FMA contraction: matches scalar Perl arithmetic bit-for-bit. */
int
Perl_mt_ndarray_kernel_slice(UV src_id, UV dst_id, UV start, UV len,
                             double alpha, double beta)
{
    const double *src;
    double *dst;
    uint64_t src_n, dst_n, i, end;

    pthread_mutex_lock(&S_nd_lock);
    perl_mt_ndarray_t *s = S_nd_find(src_id);
    perl_mt_ndarray_t *d = S_nd_find(dst_id);
    if (!s || !d) {
        pthread_mutex_unlock(&S_nd_lock);
        return -ENOENT;
    }
    if (d->sealed) {
        pthread_mutex_unlock(&S_nd_lock);
        return -EACCES;
    }
    src   = (const double *)s->shm.base;
    src_n = s->nelem;
    dst   = (double *)d->shm.base;
    dst_n = d->nelem;
    pthread_mutex_unlock(&S_nd_lock);

    end = (uint64_t)start + (uint64_t)len;
    if (end > src_n || end > dst_n)
        return -ERANGE;

    for (i = (uint64_t)start; i < end; i++) {
        volatile double x  = src[i];
        volatile double t1 = x * alpha;
        volatile double t2 = t1 + beta;
        volatile double t3 = x + 0.5;
        dst[i] = t2 * t3;
    }
    return 0;
}

double
Perl_mt_ndarray_sum_slice(UV id, UV start, UV len)
{
    const double *src;
    uint64_t n, i, end;
    double sum = 0.0;
    double c = 0.0;

    pthread_mutex_lock(&S_nd_lock);
    perl_mt_ndarray_t *nd = S_nd_find(id);
    if (!nd) {
        pthread_mutex_unlock(&S_nd_lock);
        return 0.0;
    }
    src = (const double *)nd->shm.base;
    n   = nd->nelem;
    pthread_mutex_unlock(&S_nd_lock);

    end = (uint64_t)start + (uint64_t)len;
    if (end > n) end = n;

    /* Neumaier compensated summation in deterministic index order */
    for (i = (uint64_t)start; i < end; i++) {
        double v = src[i];
        double t = sum + v;
        if (fabs(sum) >= fabs(v))
            c += (sum - t) + v;
        else
            c += (v - t) + sum;
        sum = t;
    }
    return sum + c;
}

double
Perl_mt_ndarray_dot_slice(UV a_id, UV b_id, UV start, UV len)
{
    const double *a, *b;
    uint64_t na, nb, i, end;
    double sum = 0.0;
    double c = 0.0;

    pthread_mutex_lock(&S_nd_lock);
    perl_mt_ndarray_t *nda = S_nd_find(a_id);
    perl_mt_ndarray_t *ndb = S_nd_find(b_id);
    if (!nda || !ndb) {
        pthread_mutex_unlock(&S_nd_lock);
        return 0.0;
    }
    a  = (const double *)nda->shm.base;
    na = nda->nelem;
    b  = (const double *)ndb->shm.base;
    nb = ndb->nelem;
    pthread_mutex_unlock(&S_nd_lock);

    end = (uint64_t)start + (uint64_t)len;
    if (end > na) end = na;
    if (end > nb) end = nb;

    for (i = (uint64_t)start; i < end; i++) {
        volatile double prod = a[i] * b[i];
        double v = prod;
        double t = sum + v;
        if (fabs(sum) >= fabs(v))
            c += (sum - t) + v;
        else
            c += (v - t) + sum;
        sum = t;
    }
    return sum + c;
}

SV *
Perl_mt_ndarray_slice_be_bytes(pTHX_ UV id, UV start, UV len)
{
    const double *src;
    uint64_t n, i, end, count;
    SV *sv;
    uint8_t *out;

    pthread_mutex_lock(&S_nd_lock);
    perl_mt_ndarray_t *nd = S_nd_find(id);
    if (!nd) {
        pthread_mutex_unlock(&S_nd_lock);
        Perl_croak(aTHX_ "MT::PDLBridge: unknown ndarray id %" UVuf, id);
    }
    src = (const double *)nd->shm.base;
    n   = nd->nelem;
    pthread_mutex_unlock(&S_nd_lock);

    if ((uint64_t)start >= n || len == 0)
        return newSVpvs("");
    end = (uint64_t)start + (uint64_t)len;
    if (end > n) end = n;
    count = end - (uint64_t)start;

    sv = newSV((STRLEN)(count * 8ULL));
    SvPOK_only(sv);
    out = (uint8_t *)SvPVX(sv);
    for (i = 0; i < count; i++) {
        uint64_t bits = 0;
        double v = src[(uint64_t)start + i];
        memcpy(&bits, &v, sizeof(bits));
        out[i * 8 + 0] = (uint8_t)((bits >> 56) & 0xFFu);
        out[i * 8 + 1] = (uint8_t)((bits >> 48) & 0xFFu);
        out[i * 8 + 2] = (uint8_t)((bits >> 40) & 0xFFu);
        out[i * 8 + 3] = (uint8_t)((bits >> 32) & 0xFFu);
        out[i * 8 + 4] = (uint8_t)((bits >> 24) & 0xFFu);
        out[i * 8 + 5] = (uint8_t)((bits >> 16) & 0xFFu);
        out[i * 8 + 6] = (uint8_t)((bits >>  8) & 0xFFu);
        out[i * 8 + 7] = (uint8_t)( bits        & 0xFFu);
    }
    SvCUR_set(sv, (STRLEN)(count * 8ULL));
    out[count * 8] = '\0';
    return sv;
}

HV *
Perl_mt_ndarray_info_hv(pTHX_ UV id)
{
    perl_mt_ndarray_t *nd;
    HV *hv = NULL;

    pthread_mutex_lock(&S_nd_lock);
    nd = S_nd_find(id);
    if (nd) {
        AV *dims_av = newAV();
        uint32_t d;
        for (d = 0; d < nd->ndims; d++)
            av_push(dims_av, newSVuv((UV)nd->dims[d]));
        hv = newHV();
        (void)hv_stores(hv, "id",       newSVuv((UV)nd->id));
        (void)hv_stores(hv, "gen",      newSVuv((UV)nd->gen));
        (void)hv_stores(hv, "dtype",    newSVpv(nd->dtype == MT_NDARRAY_DTYPE_I64 ? "i64" : "f64", 0));
        (void)hv_stores(hv, "nelem",    newSVuv((UV)nd->nelem));
        (void)hv_stores(hv, "sealed",   newSViv(nd->sealed ? 1 : 0));
        (void)hv_stores(hv, "shm_len",  newSVuv((UV)nd->shm.len));
        (void)hv_stores(hv, "dims",     newRV_noinc((SV *)dims_av));
    }
    pthread_mutex_unlock(&S_nd_lock);
    if (!hv)
        Perl_croak(aTHX_ "MT::PDLBridge: unknown ndarray id %" UVuf, id);
    return hv;
}

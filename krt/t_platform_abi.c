/*    t_platform_abi.c
 *
 *    Verify krt & mt_ctx ABI invariants, platform dispatch macros,
 *    byte-order helpers, and ISO-8859-1 <-> IBM-1047 (EBCDIC) tables.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#include <stdalign.h>
#include <stdint.h>
#include <string.h>
#include "krt.h"
#include "krt_internal.h"
#include "krt_test.h"
#include "../mt_ctx.h"

int
main(void)
{
    int tag_count = 0;
    const char *plat = perl_krt_platform();
    size_t pgsz = perl_krt_page_size();
    int ncpus = perl_krt_cpu_count();

#ifdef PERL_KRT_LINUX
    tag_count++;
#endif
#ifdef PERL_KRT_DARWIN
    tag_count++;
#endif
#ifdef PERL_KRT_AIX
    tag_count++;
#endif
#ifdef PERL_KRT_ZOS
    tag_count++;
#endif
#ifdef PERL_KRT_UNKNOWN
    tag_count++;
#endif

    is_int(tag_count, 1, "exactly one PERL_KRT_<OS> tag is defined");
    ok(PERL_KRT_CACHE_LINE >= 64 &&
       (PERL_KRT_CACHE_LINE & (PERL_KRT_CACHE_LINE - 1)) == 0,
       "PERL_KRT_CACHE_LINE (%d) is a power of two >= 64",
       (int)PERL_KRT_CACHE_LINE);

    /* ABI struct sizes and alignments */
    is_int((long long)sizeof(mt_ctx), (long long)sizeof(void *),
           "mt_ctx is a single pointer (%zu bytes)", sizeof(mt_ctx));
    ok(sizeof(perl_krt_shm_t) >= 40,
       "perl_krt_shm_t size (%zu)", sizeof(perl_krt_shm_t));
    ok(sizeof(perl_krt_comp_t) >= 16,
       "perl_krt_comp_t size (%zu)", sizeof(perl_krt_comp_t));
    is_int((long long)sizeof(perl_krt_stat_t), 112,
           "perl_krt_stat_t is 14 x 8 = 112 bytes");
    is_int((long long)sizeof(perl_krt_sqe_t), 64,
           "perl_krt_sqe_t is 64 bytes");
    is_int((long long)sizeof(perl_krt_cqe_t), 16,
           "perl_krt_cqe_t is 16 bytes");
    ok(sizeof(perl_krt_aio_t) >= 24,
       "perl_krt_aio_t size (%zu)", sizeof(perl_krt_aio_t));

    /* Dispatch strings and runtime probes */
    ok(plat != NULL && plat[0] != '\0',
       "perl_krt_platform() = %s", plat ? plat : "(null)");
    ok(PERL_MT_CTX_ABI[0] != '\0',
       "PERL_MT_CTX_ABI = %s", PERL_MT_CTX_ABI);
    ok(pgsz >= 4096 && (pgsz & (pgsz - 1)) == 0,
       "perl_krt_page_size() = %zu is power of two >= 4096", pgsz);
    ok(ncpus >= 1, "perl_krt_cpu_count() = %d >= 1", ncpus);

    /* Endianness consistency and byte-swap helpers */
    {
        uint32_t probe = 0x01020304u;
        const unsigned char *b = (const unsigned char *)&probe;
        int runtime_be = (b[0] == 0x01u) ? 1 : 0;
        is_int(PERL_KRT_BIG_ENDIAN, runtime_be,
               "PERL_KRT_BIG_ENDIAN matches runtime byte order");
        is_int((long long)krt_bswap16(0x1234u), 0x3412ll, "krt_bswap16");
        is_int((long long)krt_bswap32(0x12345678u), 0x78563412ll, "krt_bswap32");
        ok(krt_bswap64(0x0102030405060708ull) == 0x0807060504030201ull,
           "krt_bswap64");
        is_int((long long)krt_le32toh(krt_htole32(0xdeadbeefu)),
               (long long)0xdeadbeefu, "htole32/le32toh round-trip");
        is_int((long long)krt_be32toh(krt_htobe32(0xdeadbeefu)),
               (long long)0xdeadbeefu, "htobe32/be32toh round-trip");
        ok(krt_le64toh(krt_htole64(0x0123456789abcdefull)) == 0x0123456789abcdefull,
           "htole64/le64toh round-trip");
        ok(krt_be64toh(krt_htobe64(0x0123456789abcdefull)) == 0x0123456789abcdefull,
           "htobe64/be64toh round-trip");
    }

    /* EBCDIC (IBM-1047) <-> ASCII (ISO-8859-1) table verification */
    {
        int i, bijective_a2e = 1, bijective_e2a = 1;
        for (i = 0; i < 256; i++) {
            uint8_t c = (uint8_t)i;
            if (krt_ebcdic_1047_to_ascii(krt_ascii_to_ebcdic_1047(c)) != c)
                bijective_a2e = 0;
            if (krt_ascii_to_ebcdic_1047(krt_ebcdic_1047_to_ascii(c)) != c)
                bijective_e2a = 0;
        }
        ok(bijective_a2e, "ISO-8859-1 -> IBM-1047 -> ISO-8859-1 is bijective");
        ok(bijective_e2a, "IBM-1047 -> ISO-8859-1 -> IBM-1047 is bijective");
        is_int(PERL_KRT_EBCDIC, krt_native_is_ebcdic(),
               "PERL_KRT_EBCDIC matches runtime charset");

        /* Critical IBM-1047 code points */
        is_int(krt_ascii_to_ebcdic_1047(0x00u), 0x00, "NUL -> 0x00");
        is_int(krt_ascii_to_ebcdic_1047(0x0Au), 0x15, "LF  -> 0x15");
        is_int(krt_ascii_to_ebcdic_1047(0x20u), 0x40, "SPC -> 0x40");
        is_int(krt_ascii_to_ebcdic_1047(0x2Fu), 0x61, "'/' -> 0x61");
        is_int(krt_ascii_to_ebcdic_1047(0x30u), 0xF0, "'0' -> 0xF0");
        is_int(krt_ascii_to_ebcdic_1047(0x39u), 0xF9, "'9' -> 0xF9");
        is_int(krt_ascii_to_ebcdic_1047(0x41u), 0xC1, "'A' -> 0xC1");
        is_int(krt_ascii_to_ebcdic_1047(0x5Au), 0xE9, "'Z' -> 0xE9");
        is_int(krt_ascii_to_ebcdic_1047(0x61u), 0x81, "'a' -> 0x81");
        is_int(krt_ascii_to_ebcdic_1047(0x7Au), 0xA9, "'z' -> 0xA9");
        is_int(krt_ascii_to_ebcdic_1047(0x5Bu), 0xAD, "'[' -> 0xAD");
        is_int(krt_ascii_to_ebcdic_1047(0x5Cu), 0xE0, "'\\' -> 0xE0");
        is_int(krt_ascii_to_ebcdic_1047(0x5Du), 0xBD, "']' -> 0xBD");
        is_int(krt_ascii_to_ebcdic_1047(0x5Eu), 0x5F, "'^' -> 0x5F");
        is_int(krt_ascii_to_ebcdic_1047(0x7Bu), 0xC0, "'{' -> 0xC0");
        is_int(krt_ascii_to_ebcdic_1047(0x7Du), 0xD0, "'}' -> 0xD0");
        is_int(krt_ascii_to_ebcdic_1047(0x7Eu), 0xA1, "'~' -> 0xA1");
    }

    done_testing();
}

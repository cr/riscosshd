/* mbedTLS hardware entropy hook backed by the RISC OS CryptRandom module.
 * Enabled by MBEDTLS_ENTROPY_HARDWARE_ALT in deps/mbedtls_riscos_config.h.
 */
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <kernel.h>
#include <swis.h>

#include "riscos_entropy.h"

#ifndef CryptRandom_Byte
#define CryptRandom_Byte  0x51980
#define CryptRandom_Block 0x51983
#endif

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    *olen = 0;
    if (_swix(CryptRandom_Block, _INR(0, 1), output, len) == NULL) {
        *olen = len;
        return 0;
    }
    /* Older module without CryptRandom_Block: fetch byte by byte. */
    for (size_t i = 0; i < len; i++) {
        int b = 0;
        if (_swix(CryptRandom_Byte, _OUT(0), &b) != NULL)
            return -1; /* MBEDTLS_ERR_ENTROPY_SOURCE_FAILED is raised by the caller */
        output[i] = (unsigned char)b;
    }
    *olen = len;
    return 0;
}

/* Start-up check: 32 bytes through the hook above, or failure. */
int riscos_entropy_check(void)
{
    unsigned char buf[32];
    size_t got = 0;
    if (mbedtls_hardware_poll(NULL, buf, sizeof buf, &got) != 0 || got != sizeof buf)
        return -1;
    return 0;
}

/* Millisecond clock for MBEDTLS_PLATFORM_MS_TIME_ALT.  Only mbedTLS's TLS
 * and PSA code call it, neither of which is linked; defining the ALT keeps
 * platform_util.c from needing a POSIX clock of its own. */
typedef int64_t mbedtls_ms_time_t;
mbedtls_ms_time_t mbedtls_ms_time(void);
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (mbedtls_ms_time_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

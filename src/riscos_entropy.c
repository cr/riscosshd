/* mbedTLS hardware entropy hook backed by the RISC OS CryptRandom module.
 * Enabled by MBEDTLS_ENTROPY_HARDWARE_ALT in deps/mbedtls_riscos_config.h.
 */
#include <stddef.h>
#include <kernel.h>
#include <swis.h>

#ifndef CryptRandom_Byte
#define CryptRandom_Byte  0x51980
#define CryptRandom_Block 0x51983
#endif

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

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

/* Millisecond clock for mbedTLS (MBEDTLS_PLATFORM_MS_TIME_ALT). */
#include <stdint.h>
#include <sys/time.h>
typedef int64_t mbedtls_ms_time_t;
mbedtls_ms_time_t mbedtls_ms_time(void);
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (mbedtls_ms_time_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

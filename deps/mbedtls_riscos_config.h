/* mbedTLS user configuration for RISC OS (GCCSDK / UnixLib).
 * Included after mbedtls/mbedtls_config.h via MBEDTLS_USER_CONFIG_FILE.
 *
 * Entropy: there is no /dev/urandom on RISC OS, so the platform entropy
 * source is disabled and mbedtls_hardware_poll() (src/riscos_entropy.c)
 * feeds bytes from the CryptRandom module instead.
 */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* net_sockets.c and timing.c only build on Unix/Windows; libssh does not
 * use either. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C

/* GCCSDK targets ARMv3 by default; the bignum inline assembly needs the
 * long-multiply instructions of later cores, so use plain C. */
#undef MBEDTLS_HAVE_ASM
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_AESCE_C
#undef MBEDTLS_PADLOCK_C

/* mbedtls_ms_time() is provided by src/riscos_entropy.c (gettimeofday). */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

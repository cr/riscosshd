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

/* Size: leave out what the server never uses.  libssh needs AES (CBC, CTR,
 * GCM), ChaCha20-Poly1305, MD5/SHA-1/SHA-2 through the md layer, RSA,
 * ECDSA/ECDH on the NIST curves, Curve25519, bignum for its own DH, PEM and
 * PK parsing/writing of unencrypted keys, CTR_DRBG and the entropy pool.
 * Everything below is outside that set.  mbedtls_strerror() stays as the
 * dummy that returns a fixed string. */
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_VERSION_FEATURES
#undef MBEDTLS_ERROR_C
#undef MBEDTLS_PSA_CRYPTO_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_LMS_C
#undef MBEDTLS_SHA3_C
#undef MBEDTLS_ARIA_C
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_DES_C
#undef MBEDTLS_RIPEMD160_C
#undef MBEDTLS_CCM_C
#undef MBEDTLS_CMAC_C
#undef MBEDTLS_NIST_KW_C
#undef MBEDTLS_HKDF_C
#undef MBEDTLS_DHM_C
#undef MBEDTLS_ECJPAKE_C
#undef MBEDTLS_PKCS5_C
#undef MBEDTLS_PKCS12_C
#undef MBEDTLS_CIPHER_MODE_CFB
#undef MBEDTLS_CIPHER_MODE_OFB
#undef MBEDTLS_CIPHER_MODE_XTS
#undef MBEDTLS_ECP_DP_SECP192R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP192K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP256K1_ENABLED
#undef MBEDTLS_ECP_DP_BP256R1_ENABLED
#undef MBEDTLS_ECP_DP_BP384R1_ENABLED
#undef MBEDTLS_ECP_DP_BP512R1_ENABLED
#undef MBEDTLS_ECP_DP_CURVE448_ENABLED

/* The TLS library is not linked, so its options cost nothing; only the
 * ones whose prerequisites went above are switched off. */
#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_EARLY_DATA
#undef MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED

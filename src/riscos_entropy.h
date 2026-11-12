#ifndef SSHD_RISCOS_ENTROPY_H
#define SSHD_RISCOS_ENTROPY_H
#include <stddef.h>
/* mbedTLS hardware entropy hook, fed by the CryptRandom module. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
/* 0 if the module answers, -1 if it is absent or failing. */
int riscos_entropy_check(void);
#endif

/*
 * tools/tpm_linux.h — TPM transport for Linux tools: a TPM character device or a unix socket (swtpm).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TPM_LINUX_H
#define TPM_LINUX_H

#include "../include/rwy_tpm_cmd.h"

/* Open DEV (a character device such as /dev/tpmrm0, or a unix socket path); 0 or -1 with errno set. */
int tpm_linux_open(rwy_tpm_xport_t *x, const char *dev);
void tpm_linux_close(rwy_tpm_xport_t *x);

#endif

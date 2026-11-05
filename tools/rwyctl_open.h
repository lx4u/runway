/* rwyctl_open.h: shared key opening for rwyctl and rwy-fuse. */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RWYCTL_OPEN_H
#define RWYCTL_OPEN_H

#include <stddef.h>
#include <stdint.h>
#include "rwy_block.h"

extern const char *g_passfile, *g_dekfile, *g_tpm;
extern const char *g_passstring;

void warn(const char *fmt, const char *a);
int slurp(const char *path, void *buf, size_t max);
int prompt_pass(const char *prompt_str, char *out, size_t max);
const char *tpm_dev(void);
int tpm_dek_dev(rwy_block_dev_t *dev, uint8_t dek[32]);
int pass_dek_dev(rwy_block_dev_t *dev, const char *pass, uint8_t dek[32]);
int key_source(int tries, int tpm, const char *file, const char *pf, uint8_t dek[32],
               char *pass, const char **src, int *have);
int rwy_resolve_dev_key(rwy_block_dev_t *dev, const char *key_pass, uint8_t dek[32]);

#endif

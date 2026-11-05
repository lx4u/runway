/* tools/guard_linux.h: the boot loop guard counter seen from Linux, shared by rwyctl and rwy-fuse. */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GUARD_LINUX_H
#define GUARD_LINUX_H

#include <stddef.h>
#include "rwy_core.h"

/* The slot RUNWAY.EFI says it started (volatile variable RunwayBoot): 1 and its name, 0 when unknown.
 * `root` is prepended to every path (empty on a real system). */
int guard_started(const char *root, char *slot, size_t cap);
/* 1 when `name` is BOOTED or names the slot BOOTED points to; that slot goes in `booted`. */
int guard_is_booted(rwy_store_t *s, const char *name, char *booted, size_t cap);
/* Delete the RunwayBootGuard counter. 0 when it is gone or was never there, else -errno. */
int guard_clear(const char *root);
/* The health point: `name` was just written OK. Clears the counter when it is the slot that started.
 * 0 when cleared or not applicable, else -errno of a clear that failed. */
int guard_healthy(rwy_store_t *s, const char *root, const char *name);

#endif

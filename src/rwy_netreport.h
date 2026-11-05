/*
 * rwy_netreport.h — Send this boot's not-normal flight events to the syslog server.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_NETREPORT_H
#define RWY_NETREPORT_H

#include <efi.h>
#include "../include/rwy_core.h"

/* Send the not-normal events of this boot, then the newest committed ring events (LOG-BACKLOG), when the
 * settings name a server, an interface and an address. Returns when the budget (LOG-TIMEOUT) is spent at
 * the latest; it never changes the boot. */
void rwy_netreport_send(rwy_store_t *s);

/* Send the STORE event of a boot whose store cannot be mounted, with the first complete settings of the
 * members. The ring is not read: it is behind the key. */
void rwy_netreport_send_unmounted(const rwy_block_dev_t devs[], const rwy_superblock_t sbs[], UINTN count);

#endif /* RWY_NETREPORT_H */

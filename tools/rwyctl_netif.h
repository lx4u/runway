/*
 * rwyctl_netif.h — Host network interfaces for the flight report settings (Linux).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWYCTL_NETIF_H
#define RWYCTL_NETIF_H

#include <net/if.h>
#include <stddef.h>
#include <stdint.h>

int netif_perm_mac(const char *name, uint8_t mac[6]);                  /* permanent address; 0, or -1 */
int netif_by_perm_mac(const uint8_t mac[6], char name[IFNAMSIZ]);      /* local interface with it; 0, or -1 */
int netif_ipv4(const char *name, uint32_t *addr, uint8_t *prefix);     /* first IPv4 address; 0, or -1 */
int netif_gateway(const char *name, uint32_t *gw);                     /* default gateway, 0 if none; 0, or -1 */
int netif_send(const char *name, uint32_t src, uint32_t dst, uint16_t port, const char *msg, size_t len);

#endif /* RWYCTL_NETIF_H */

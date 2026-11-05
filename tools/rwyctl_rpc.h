/* tools/rwyctl_rpc.h: client/server RPC protocol between rwyctl and rwy-fuse. */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RWYCTL_RPC_H
#define RWYCTL_RPC_H

#include <stdint.h>

/* Connect to the RPC socket of the store. Returns fd or -1. */
int rwyctl_rpc_connect(const uint8_t uuid[16]);

/* True when the socket of a daemon for the store exists. */
int rwyctl_rpc_available(const uint8_t uuid[16]);

/* Client side: execute argc, argv over RPC. Returns command exit code, or -1 if offline. */
int rwyctl_rpc_client(const uint8_t uuid[16], int argc, char **argv);

/* Server side: receive and execute request, send exit code back. */
void rwyctl_rpc_server_handle(int conn);

#endif

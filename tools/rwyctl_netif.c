/*
 * rwyctl_netif.c — Host network interfaces for the flight report settings (Linux): the permanent MAC,
 * the IPv4 address and default gateway of an interface, and one datagram sent from it.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rwyctl_netif.h"

int netif_perm_mac(const char *name, uint8_t mac[6]) {
    struct {
        struct ethtool_perm_addr hdr;
        uint8_t addr[6];
    } req;
    struct ifreq ifr;
    int fd, rc;

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&req, 0, sizeof(req));
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    req.hdr.cmd = ETHTOOL_GPERMADDR;
    req.hdr.size = sizeof(req.addr);
    ifr.ifr_data = (void *)&req.hdr;
    rc = ioctl(fd, SIOCETHTOOL, &ifr);
    close(fd);
    if (rc || req.hdr.size != sizeof(req.addr)) return -1;
    for (unsigned i = 0; i < sizeof(req.addr); i++)
        if (req.addr[i]) {
            memcpy(mac, req.addr, sizeof(req.addr));
            return 0;
        }
    return -1;
}

int netif_by_perm_mac(const uint8_t mac[6], char name[IFNAMSIZ]) {
    struct ifaddrs *list, *ifa;
    uint8_t perm[6];
    int rc = -1;

    if (getifaddrs(&list)) return -1;
    for (ifa = list; ifa && rc; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_PACKET) continue;
        if (netif_perm_mac(ifa->ifa_name, perm) || memcmp(perm, mac, 6)) continue;
        snprintf(name, IFNAMSIZ, "%s", ifa->ifa_name);
        rc = 0;
    }
    freeifaddrs(list);
    return rc;
}

int netif_ipv4(const char *name, uint32_t *addr, uint8_t *prefix) {
    struct ifaddrs *list, *ifa;
    int rc = -1;

    if (getifaddrs(&list)) return -1;
    for (ifa = list; ifa && rc; ifa = ifa->ifa_next) {
        const struct sockaddr_in *sin = (const void *)ifa->ifa_addr;
        const struct sockaddr_in *mask = (const void *)ifa->ifa_netmask;

        if (!sin || !mask || sin->sin_family != AF_INET || strcmp(ifa->ifa_name, name)) continue;
        *addr = ntohl(sin->sin_addr.s_addr);
        *prefix = (uint8_t)__builtin_popcount(ntohl(mask->sin_addr.s_addr));
        rc = 0;
    }
    freeifaddrs(list);
    return rc;
}

/* The gateway is the raw network-order word of /proc/net/route, read on a little-endian host. */
int netif_gateway(const char *name, uint32_t *gw) {
    char line[256], iface[IFNAMSIZ];
    unsigned dst, gate, mask;
    FILE *f;

    *gw = 0;
    f = fopen("/proc/net/route", "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%15s %x %x %*x %*d %*d %*d %x", iface, &dst, &gate, &mask) != 4) continue;
        if (!strcmp(iface, name) && dst == 0 && mask == 0) {
            *gw = ntohl(gate);
            break;
        }
    }
    fclose(f);
    return 0;
}

static int send_from(int fd, const char *name, uint32_t src, uint32_t dst, uint16_t port, const char *msg,
                     size_t len) {
    struct sockaddr_in sin;
    int one = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name, strlen(name))) return -1;
    if (setsockopt(fd, IPPROTO_IP, IP_FREEBIND, &one, sizeof(one))) return -1;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(src);
    if (bind(fd, (const struct sockaddr *)&sin, sizeof(sin))) return -1;
    sin.sin_addr.s_addr = htonl(dst);
    sin.sin_port = htons(port);
    return sendto(fd, msg, len, 0, (const struct sockaddr *)&sin, sizeof(sin)) == (ssize_t)len ? 0 : -1;
}

int netif_send(const char *name, uint32_t src, uint32_t dst, uint16_t port, const char *msg, size_t len) {
    int fd, rc;

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    rc = send_from(fd, name, src, dst, port, msg, len);
    close(fd);
    return rc;
}

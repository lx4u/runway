/*
 * rwyctl_log.c — `rwyctl log settings|set|unset`: the plaintext settings of the
 * network flight report. The area is outside the encrypted range, so the
 * commands need no keys.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "rwy_netset.h"
#include "rwyctl.h"
#include "rwyctl_netif.h"

static int live_members(const group_t *g) {
    int n = 0;
    for (int i = 0; i < g->n; i++) n += g_mem[g->m[i]].ok;
    return n;
}

static void print_copy(char name, int status, uint64_t seq) {
    if (status == RWY_LOG_OK) printf("  copy %c: valid, sequence %llu\n", name, (unsigned long long)seq);
    else printf("  copy %c: %s\n", name, rwy_log_reason(status));
}

static int member_show(const member_t *m) {
    rwy_log_info_t info;
    char text[512];
    int rc;

    if (!m->net_blocks) {
        printf("member %s: no settings area\n", m->path);
        return X_POLICY;
    }
    printf("member %s: settings area 2 x %u blocks at %u\n", m->path, m->net_blocks, m->net_start);
    rc = rwy_log_pick(&m->raw, m->net_start, m->net_blocks, &info);
    print_copy('A', info.status[0], info.seq[0]);
    print_copy('B', info.status[1], info.seq[1]);
    if (rc == RWY_OK) {
        rwy_log_format(&info.settings, text, sizeof(text));
        fputs(text, stdout);
        return X_OK_;
    }
    if (info.status[0] == RWY_LOG_EMPTY && info.status[1] == RWY_LOG_EMPTY) {
        rwy_log_settings_t s;
        rwy_log_settings_init(&s);
        rwy_log_format(&s, text, sizeof(text));
        fputs(text, stdout);
        return X_OK_;
    }
    warn("member %s: settings area corrupted", m->path);
    return X_CHECK;
}

static int settings_show(group_t *g) {
    int rc = X_OK_;
    int n = 0;

    for (int i = 0; i < g->n; i++) {
        const member_t *m = &g_mem[g->m[i]];
        int mrc;
        if (!m->ok) continue;
        n++;
        mrc = member_show(m);
        if (mrc != X_OK_) rc = mrc;
    }
    return n ? rc : X_IO;
}

static int apply_one(const member_t *m, rwy_log_setting_t id, const char *value, int unset) {
    rwy_log_info_t info;
    rwy_log_settings_t s;
    const char *why = NULL;

    if (!m->net_blocks) {
        warn("member %s: no settings area", m->path);
        return -1;
    }
    if (rwy_log_pick(&m->raw, m->net_start, m->net_blocks, &info) == RWY_OK) s = info.settings;
    else rwy_log_settings_init(&s);

    if (unset) {
        if (rwy_log_unset(&s, id, &why)) {
            warn("member %s: %s", why ? why : "cannot unset the setting");
            return -1;
        }
    } else if (rwy_log_set(&s, id, value, strlen(value), &why)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: %s", rwy_log_setting_name(id), why ? why : "invalid value");
        warn("member %s: invalid setting", m->path);
        warn("%s", msg);
        return -1;
    }
    if (!rwy_log_settings_valid(&s, &why)) {
        warn("member %s: %s", why ? why : "invalid settings");
        return -1;
    }
    if (rwy_log_write(&m->raw, m->net_start, m->net_blocks, &s)) {
        warn("member %s: cannot write the settings area", m->path);
        return -1;
    }
    return 0;
}

static int settings_change(group_t *g, rwy_log_setting_t id, const char *value, int unset) {
    int rc, wanted, written = 0;

    if ((rc = lock_group(g))) return rc;
    wanted = live_members(g);
    for (int i = 0; i < g->n; i++) {
        const member_t *m = &g_mem[g->m[i]];
        if (!m->ok) continue;
        if (apply_one(m, id, value, unset) == 0) written++;
    }
    rc = (written == wanted && wanted) ? X_OK_ : X_IO;
    if (written != wanted) warn("%s", "degraded: not every member was written");
    return rc;
}

static int parse_name(const char *name, rwy_log_setting_t *id) {
    int v = rwy_log_setting_id(name);
    if (v < 0) {
        warn("unknown setting %s (LOG-SERVER, LOG-INTERFACE, LOG-ADDRESS, LOG-TIMEOUT, LOG-BACKLOG)", name);
        return X_USAGE;
    }
    *id = (rwy_log_setting_t)v;
    return 0;
}

/* The permanent MAC of a local interface, written as the setting expects it. */
static int interface_mac(const char *name, char text[18]) {
    uint8_t mac[6];
    if (netif_perm_mac(name, mac)) {
        warn("no permanent address for interface %s", name);
        return X_USAGE;
    }
    snprintf(text, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

static int log_set_cmd(group_t *g, int argc, char **argv) {
    rwy_log_setting_t id;
    char mac[18];
    const char *value;
    int rc;
    if (argc != 2) { warn("%s", "usage: log set NAME VALUE"); return X_USAGE; }
    if ((rc = parse_name(argv[0], &id))) return rc;
    value = argv[1];
    if (id == RWY_LOG_INTERFACE && !strchr(value, ':')) {
        if ((rc = interface_mac(value, mac))) return rc;
        value = mac;
    }
    return settings_change(g, id, value, 0);
}

static void ipv4_text(uint32_t addr, char *out, size_t n) {
    snprintf(out, n, "%u.%u.%u.%u", addr >> 24, (addr >> 16) & 0xff, (addr >> 8) & 0xff, addr & 0xff);
}

/* The address, prefix and gateway of a running interface, as the LOG-ADDRESS value. */
static int interface_address(const char *name, char text[64]) {
    uint32_t addr, gw;
    uint8_t prefix;
    char a[16], gwtext[16];

    if (netif_ipv4(name, &addr, &prefix)) {
        warn("no IPv4 address on interface %s", name);
        return X_USAGE;
    }
    if (netif_gateway(name, &gw)) {
        warn("cannot read the routes of interface %s", name);
        return X_IO;
    }
    ipv4_text(addr, a, sizeof(a));
    if (!gw) {
        snprintf(text, 64, "%s/%u", a, prefix);
        return 0;
    }
    ipv4_text(gw, gwtext, sizeof(gwtext));
    snprintf(text, 64, "%s/%u,%s", a, prefix, gwtext);
    return 0;
}

static int log_copy_address_cmd(group_t *g, int argc, char **argv) {
    char text[64];
    int rc;
    if (argc != 1) { warn("%s", "usage: log copy-address IFNAME"); return X_USAGE; }
    if ((rc = interface_address(argv[0], text))) return rc;
    return settings_change(g, RWY_LOG_ADDRESS, text, 0);
}

/* The settings of the first member that can be read: the feature needs all three of them. */
static int test_settings(group_t *g, rwy_log_settings_t *s) {
    const uint8_t need = RWY_LOG_SET_SERVER | RWY_LOG_SET_MAC | RWY_LOG_SET_ADDRESS;
    rwy_log_info_t info;

    for (int i = 0; i < g->n; i++) {
        const member_t *m = &g_mem[g->m[i]];
        if (!m->ok || !m->net_blocks) continue;
        if (rwy_log_pick(&m->raw, m->net_start, m->net_blocks, &info) != RWY_OK) continue;
        *s = info.settings;
        if ((s->flags & need) == need) return 0;
        warn("%s", "LOG-SERVER, LOG-INTERFACE and LOG-ADDRESS must all be set");
        return X_POLICY;
    }
    warn("%s", "no readable settings area");
    return X_CHECK;
}

/* One RFC 5424 test line (local0.info), sent from the interface with the static address to the server. */
static int log_test_cmd(group_t *g, int argc) {
    rwy_log_settings_t s;
    char name[IFNAMSIZ], uuid[37], line[256], server[16];
    size_t len;
    int rc;

    if (argc != 0) { warn("%s", "usage: log test"); return X_USAGE; }
    if ((rc = test_settings(g, &s))) return rc;
    if (netif_by_perm_mac(s.mac, name)) {
        warn("%s", "no local interface has the LOG-INTERFACE address");
        return X_CHECK;
    }
    rwy_format_uuid(g->uuid, uuid);
    ipv4_text(s.server, server, sizeof(server));
    len = (size_t)snprintf(line, sizeof(line), "<134>1 - %s runway - test - rwyctl log test", uuid);
    if (netif_send(name, s.address, s.server, s.port, line, len)) {
        warn("cannot send to %s", server);
        return X_IO;
    }
    printf("log test: sent to %s:%u from %s\n", server, s.port, name);
    return X_OK_;
}

static int log_unset_cmd(group_t *g, int argc, char **argv) {
    rwy_log_setting_t id;
    int rc;
    if (argc != 1) { warn("%s", "usage: log unset NAME"); return X_USAGE; }
    if ((rc = parse_name(argv[0], &id))) return rc;
    return settings_change(g, id, NULL, 1);
}

int cmd_log_settings(group_t *g, int argc, char **argv) {
    int live = live_members(g);
    if (!live) { warn("%s", "no store member is readable"); return X_IO; }
    if (argc == 0 || (argc == 1 && !strcmp(argv[0], "settings"))) return settings_show(g);
    if (!strcmp(argv[0], "set")) return log_set_cmd(g, argc - 1, argv + 1);
    if (!strcmp(argv[0], "unset")) return log_unset_cmd(g, argc - 1, argv + 1);
    if (!strcmp(argv[0], "copy-address")) return log_copy_address_cmd(g, argc - 1, argv + 1);
    if (!strcmp(argv[0], "test")) return log_test_cmd(g, argc - 1);
    warn("%s", "usage: log settings | log set NAME VALUE | log unset NAME | log copy-address IFNAME | log test");
    return X_USAGE;
}

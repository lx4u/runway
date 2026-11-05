/*
 * rwy_netset.c — Plaintext settings of the network flight report, shared by the
 * loader, rwyctl, fsck.runway and the FUSE view.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "rwy_internal.h"
#include "rwy_netset.h"

static const char *const NAMES[RWY_LOG_SETTING_COUNT] = {
    "LOG-SERVER", "LOG-INTERFACE", "LOG-ADDRESS", "LOG-TIMEOUT", "LOG-BACKLOG"
};

void rwy_log_settings_init(rwy_log_settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->port = RWY_LOG_SERVER_PORT;
    s->timeout = RWY_LOG_TIMEOUT_DEFAULT;
    s->backlog = RWY_LOG_BACKLOG_DEFAULT;
}

const char *rwy_log_setting_name(rwy_log_setting_t id) {
    return id >= 0 && id < RWY_LOG_SETTING_COUNT ? NAMES[id] : NULL;
}

int rwy_log_setting_id(const char *name) {
    for (int i = 0; name && i < RWY_LOG_SETTING_COUNT; i++)
        if (!rwy_strcmp(name, NAMES[i])) return i;
    return -1;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void trim_end(const char **p, size_t *n) {
    while (*n > 0 && is_space((*p)[*n - 1])) (*n)--;
}

static int dec(const char *s, size_t n, unsigned min, unsigned max, unsigned *out) {
    unsigned v = 0;
    if (!n) return 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        if (v > 100000) return 0;
        v = v * 10 + (unsigned)(s[i] - '0');
    }
    if (v < min || v > max) return 0;
    *out = v;
    return 1;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* One decimal part of a dotted address: 0 to 255, no leading zero. */
static int ipv4_part(const char *s, size_t n, size_t *i, unsigned *v) {
    size_t start = *i;

    *v = 0;
    while (*i < n && s[*i] != '.') {
        if (s[*i] < '0' || s[*i] > '9') return 0;
        *v = *v * 10 + (unsigned)(s[*i] - '0');
        if (*v > 255) return 0;
        (*i)++;
    }
    if (*i == start || *i - start > 3) return 0;
    return !(*i - start > 1 && s[start] == '0');
}

static int ipv4(const char *s, size_t n, uint32_t *out) {
    uint32_t addr = 0;
    size_t i = 0;
    unsigned v;

    if (n < 7 || n > 15) return 0;
    for (unsigned part = 0; part < 4; part++) {
        if (!ipv4_part(s, n, &i, &v)) return 0;
        addr = (addr << 8) | v;
        if (part < 3 && (i >= n || s[i++] != '.')) return 0;
    }
    if (i != n) return 0;
    *out = addr;
    return 1;
}

static int server_value(const char *s, size_t n, uint32_t *addr, uint16_t *port) {
    size_t colon = n;

    for (size_t i = 0; i < n; i++)
        if (s[i] == ':') { colon = i; break; }
    if (!ipv4(s, colon, addr) || *addr == 0) return 0;
    if (colon == n) {
        *port = RWY_LOG_SERVER_PORT;
        return 1;
    }
    unsigned p;
    if (!dec(s + colon + 1, n - colon - 1, 1, 65535, &p)) return 0;
    *port = (uint16_t)p;
    return 1;
}

static int mac_value(const char *s, size_t n, uint8_t mac[6]) {
    if (n != 17) return 0;
    for (unsigned i = 0; i < 6; i++) {
        int hi = hex_digit(s[i * 3]), lo = hex_digit(s[i * 3 + 1]);
        if (hi < 0 || lo < 0) return 0;
        if (i < 5 && s[i * 3 + 2] != ':') return 0;
        mac[i] = (uint8_t)(hi << 4 | lo);
    }
    if ((mac[0] & 1) || !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) return 0;
    return 1;
}

static int address_value(const char *s, size_t n, uint32_t *addr, uint8_t *prefix, uint32_t *gateway) {
    size_t slash = n, comma = n;

    for (size_t i = 0; i < n; i++) {
        if (s[i] == '/' && slash == n) slash = i;
        else if (s[i] == ',' && comma == n) comma = i;
    }
    if (slash == n || comma < slash) return 0;
    if (!ipv4(s, slash, addr) || *addr == 0) return 0;
    unsigned p;
    if (!dec(s + slash + 1, (comma == n ? n : comma) - slash - 1, 1, 32, &p)) return 0;
    *prefix = (uint8_t)p;
    *gateway = 0;
    if (comma != n) {
        if (!ipv4(s + comma + 1, n - comma - 1, gateway) || *gateway == 0) return 0;
    }
    return 1;
}

int rwy_log_set(rwy_log_settings_t *s, rwy_log_setting_t id, const char *value, size_t len,
                const char **why) {
    const char *p = value;
    size_t n = len;
    unsigned v;

    *why = "invalid value";
    trim_end(&p, &n);
    switch (id) {
    case RWY_LOG_SERVER: {
        uint32_t addr;
        uint16_t port;
        if (!server_value(p, n, &addr, &port)) return -1;
        s->server = addr;
        s->port = port;
        s->flags |= RWY_LOG_SET_SERVER;
        return 0;
    }
    case RWY_LOG_INTERFACE: {
        uint8_t mac[6];
        if (!mac_value(p, n, mac)) return -1;
        memcpy(s->mac, mac, sizeof(mac));
        s->flags |= RWY_LOG_SET_MAC;
        return 0;
    }
    case RWY_LOG_ADDRESS: {
        uint32_t addr, gateway;
        uint8_t prefix;
        if (!address_value(p, n, &addr, &prefix, &gateway)) return -1;
        s->address = addr;
        s->prefix = prefix;
        s->gateway = gateway;
        s->flags |= RWY_LOG_SET_ADDRESS;
        return 0;
    }
    case RWY_LOG_TIMEOUT:
        if (!dec(p, n, RWY_LOG_TIMEOUT_MIN, RWY_LOG_TIMEOUT_MAX, &v)) return -1;
        s->timeout = (uint8_t)v;
        return 0;
    case RWY_LOG_BACKLOG:
        if (!dec(p, n, 0, RWY_LOG_BACKLOG_MAX, &v)) return -1;
        s->backlog = (uint8_t)v;
        return 0;
    default:
        *why = "unknown setting";
        return -1;
    }
}

int rwy_log_unset(rwy_log_settings_t *s, rwy_log_setting_t id, const char **why) {
    *why = "unknown setting";
    switch (id) {
    case RWY_LOG_SERVER: s->flags &= (uint8_t)~RWY_LOG_SET_SERVER; return 0;
    case RWY_LOG_INTERFACE: s->flags &= (uint8_t)~RWY_LOG_SET_MAC; return 0;
    case RWY_LOG_ADDRESS: s->flags &= (uint8_t)~RWY_LOG_SET_ADDRESS; return 0;
    case RWY_LOG_TIMEOUT: s->timeout = RWY_LOG_TIMEOUT_DEFAULT; return 0;
    case RWY_LOG_BACKLOG: s->backlog = RWY_LOG_BACKLOG_DEFAULT; return 0;
    default: return -1;
    }
}

int rwy_log_settings_valid(const rwy_log_settings_t *s, const char **why) {
    *why = "invalid settings";
    if (s->flags & (uint8_t)~(RWY_LOG_SET_SERVER | RWY_LOG_SET_MAC | RWY_LOG_SET_ADDRESS)) return 0;
    if ((s->flags & RWY_LOG_SET_SERVER) && (!s->server || !s->port)) return 0;
    if ((s->flags & RWY_LOG_SET_MAC) &&
        ((s->mac[0] & 1) || !(s->mac[0] | s->mac[1] | s->mac[2] | s->mac[3] | s->mac[4] | s->mac[5])))
        return 0;
    if (s->flags & RWY_LOG_SET_ADDRESS) {
        if (!s->address || s->prefix < 1 || s->prefix > 32) return 0;
    }
    if (s->timeout < RWY_LOG_TIMEOUT_MIN || s->timeout > RWY_LOG_TIMEOUT_MAX) return 0;
    if (s->backlog > RWY_LOG_BACKLOG_MAX) return 0;
    *why = NULL;
    return 1;
}

static char *put_str(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

static char *put_u64(char *p, uint64_t v) {
    char tmp[24];
    int i = 0;
    do { tmp[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i > 0) *p++ = tmp[--i];
    return p;
}

static char *put_ipv4(char *p, uint32_t addr) {
    p = put_u64(p, (addr >> 24) & 0xff); *p++ = '.';
    p = put_u64(p, (addr >> 16) & 0xff); *p++ = '.';
    p = put_u64(p, (addr >> 8) & 0xff); *p++ = '.';
    return put_u64(p, addr & 0xff);
}

static char *put_mac(char *p, const uint8_t mac[6]) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 6; i++) {
        if (i) *p++ = ':';
        *p++ = hex[mac[i] >> 4];
        *p++ = hex[mac[i] & 0xf];
    }
    return p;
}

size_t rwy_log_format(const rwy_log_settings_t *s, char *buf, size_t bufsz) {
    char tmp[512];
    char *p = tmp;

    p = put_str(p, "LOG-SERVER=");
    if (s->flags & RWY_LOG_SET_SERVER) {
        p = put_ipv4(p, s->server);
        *p++ = ':';
        p = put_u64(p, s->port);
    } else {
        *p++ = '-';
    }
    *p++ = '\n';

    p = put_str(p, "LOG-INTERFACE=");
    if (s->flags & RWY_LOG_SET_MAC) p = put_mac(p, s->mac);
    else *p++ = '-';
    *p++ = '\n';

    p = put_str(p, "LOG-ADDRESS=");
    if (s->flags & RWY_LOG_SET_ADDRESS) {
        p = put_ipv4(p, s->address);
        *p++ = '/';
        p = put_u64(p, s->prefix);
        if (s->gateway) {
            *p++ = ',';
            p = put_ipv4(p, s->gateway);
        }
    } else {
        *p++ = '-';
    }
    *p++ = '\n';

    p = put_str(p, "LOG-TIMEOUT=");
    p = put_u64(p, s->timeout);
    *p++ = '\n';
    p = put_str(p, "LOG-BACKLOG=");
    p = put_u64(p, s->backlog);
    *p++ = '\n';

    size_t len = (size_t)(p - tmp);
    if (buf && bufsz > 0) memcpy(buf, tmp, len < bufsz ? len : bufsz);
    return len;
}

const char *rwy_log_reason(int status) {
    switch (status) {
    case RWY_LOG_OK: return "valid";
    case RWY_LOG_EMPTY: return "empty";
    case RWY_LOG_BAD: return "bad header or settings";
    case RWY_LOG_IO: return "I/O error";
    default: return "unknown";
    }
}

static void settings_from_block(const rwy_log_block_t *b, rwy_log_settings_t *s) {
    s->server = b->server;
    s->port = b->port;
    memcpy(s->mac, b->mac, sizeof(s->mac));
    s->address = b->address;
    s->gateway = b->gateway;
    s->prefix = b->prefix;
    s->timeout = b->timeout;
    s->backlog = b->backlog;
    s->flags = (uint8_t)b->flags;
}

static int block_check(const uint8_t blk[RWY_BLOCK_SIZE], unsigned copy, rwy_log_settings_t *out) {
    const rwy_blk_header_t *h = (const rwy_blk_header_t *)blk;
    const rwy_log_block_t *b = (const rwy_log_block_t *)blk;
    rwy_log_settings_t s;
    const char *why;
    int zero = 1;

    for (size_t i = 0; i < RWY_BLOCK_SIZE; i++)
        if (blk[i]) { zero = 0; break; }
    if (zero) return RWY_LOG_EMPTY;
    if (h->magic != RWY_BLK_MAGIC || h->kind != RWY_BLK_KIND_LOG || h->index != copy ||
        h->generation == 0 || b->version != RWY_LOG_VERSION ||
        (b->flags & ~(uint32_t)(RWY_LOG_SET_SERVER | RWY_LOG_SET_MAC | RWY_LOG_SET_ADDRESS)) ||
        rwy_blk_compute_crc(blk) != h->crc32c)
        return RWY_LOG_BAD;
    settings_from_block(b, &s);
    if (!rwy_log_settings_valid(&s, &why)) return RWY_LOG_BAD;
    if (out) *out = s;
    return RWY_LOG_OK;
}

int rwy_log_pick(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks, rwy_log_info_t *info) {
    uint8_t blk[RWY_BLOCK_SIZE];
    rwy_log_settings_t s;
    int have = 0;

    if (!dev || !info || !blocks) return RWY_ERR_INVAL;
    memset(info, 0, sizeof(*info));
    rwy_log_settings_init(&info->settings);
    for (unsigned c = 0; c < 2; c++) {
        if (rwy_blk_read(dev, RWY_IO_META, start + c * blocks, blk)) {
            info->status[c] = RWY_LOG_IO;
            continue;
        }
        info->status[c] = block_check(blk, c, &s);
        if (info->status[c] != RWY_LOG_OK) continue;
        info->seq[c] = ((const rwy_blk_header_t *)blk)->generation;
        if (!have || info->seq[c] > info->sequence) {
            info->copy = c;
            info->sequence = info->seq[c];
            info->settings = s;
            have = 1;
        }
    }
    if (info->status[0] != RWY_LOG_OK && info->status[1] != RWY_LOG_OK)
        return (info->status[0] == RWY_LOG_IO || info->status[1] == RWY_LOG_IO) ? RWY_ERR_IO : RWY_ERR_CORRUPT;
    return RWY_OK;
}

static void block_fill(uint8_t blk[RWY_BLOCK_SIZE], unsigned copy, uint64_t seq,
                       const rwy_log_settings_t *s) {
    rwy_log_block_t *b = (rwy_log_block_t *)blk;

    memset(blk, 0, RWY_BLOCK_SIZE);
    rwy_blk_hdr_make(&b->hdr, seq, copy, RWY_BLK_KIND_LOG);
    b->version = RWY_LOG_VERSION;
    b->flags = s->flags;
    b->server = s->server;
    b->port = s->port;
    memcpy(b->mac, s->mac, sizeof(b->mac));
    b->address = s->address;
    b->gateway = s->gateway;
    b->prefix = s->prefix;
    b->timeout = s->timeout;
    b->backlog = s->backlog;
    b->hdr.crc32c = rwy_blk_compute_crc(blk);
}

int rwy_log_write(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                  const rwy_log_settings_t *settings) {
    rwy_log_info_t info;
    uint8_t blk[RWY_BLOCK_SIZE];
    unsigned target;
    uint64_t seq;
    const char *why;

    if (!dev || !blocks || !settings) return RWY_ERR_INVAL;
    if (!rwy_log_settings_valid(settings, &why)) return RWY_ERR_INVAL;
    if (rwy_log_pick(dev, start, blocks, &info) == RWY_OK) {
        target = info.copy ^ 1;
        seq = info.sequence + 1;
    } else {
        target = 0;
        seq = 1;
    }
    block_fill(blk, target, seq, settings);
    if (rwy_blk_write(dev, RWY_IO_META, start + target * blocks, blk)) return RWY_ERR_IO;
    return rwy_dev_flush(dev);
}

static void vread_slice(const char *line, size_t line_len, uint64_t off, uint64_t pos,
                        void *buf, size_t req_len, size_t *got) {
    size_t start_in_line, copy_bytes;

    if (!buf || *got >= req_len || off + line_len <= pos || off >= pos + req_len) return;
    start_in_line = pos > off ? (size_t)(pos - off) : 0;
    copy_bytes = line_len - start_in_line;
    if (*got + copy_bytes > req_len) copy_bytes = req_len - *got;
    memcpy((char *)buf + *got, line + start_in_line, copy_bytes);
    *got += copy_bytes;
}

int rwy_log_vread(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                  uint64_t pos, void *buf, size_t *len, size_t *total_sz) {
    rwy_log_info_t info;
    char line[512];
    size_t line_len, got = 0, req_len = (buf && len) ? *len : 0;

    if (!dev || !blocks) {
        if (total_sz) *total_sz = 0;
        if (len) *len = 0;
        return RWY_OK;
    }
    if (rwy_log_pick(dev, start, blocks, &info) != RWY_OK) rwy_log_settings_init(&info.settings);
    line_len = rwy_log_format(&info.settings, line, sizeof(line));
    vread_slice(line, line_len, 0, pos, buf, req_len, &got);
    if (total_sz) *total_sz = line_len;
    if (len) *len = got;
    return RWY_OK;
}

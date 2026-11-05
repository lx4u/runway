/*
 * netsettest.c — Network flight report settings: parsers, defaults, formatting,
 * A/B ping-pong, torn write and the read-only view.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rwy_block.h"
#include "rwy_format.h"
#include "rwy_internal.h"
#include "rwy_netset.h"

#define BASE_BLOCK   8U
#define COPY_BLOCKS  1U
#define TOTAL_BLOCKS (BASE_BLOCK + 2 * COPY_BLOCKS + 2)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

typedef struct {
    uint8_t mem[TOTAL_BLOCKS * RWY_BLOCK_SIZE];
    int writes;
    int cut_after;
    int flushes;
} mem_t;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_t *m = ctx;
    if (off + len > sizeof(m->mem)) return -1;
    memcpy(buf, m->mem + off, len);
    return 0;
}

static int mem_write(void *ctx, uint64_t off, const void *buf, size_t len) {
    mem_t *m = ctx;
    if (off + len > sizeof(m->mem)) return -1;
    if (m->cut_after >= 0 && m->writes >= m->cut_after) return -1;
    memcpy(m->mem + off, buf, len);
    m->writes++;
    return 0;
}

static int mem_flush(void *ctx) {
    ((mem_t *)ctx)->flushes++;
    return 0;
}

static void mem_init(mem_t *m, rwy_block_dev_t *d) {
    memset(m, 0, sizeof(*m));
    m->cut_after = -1;
    memset(d, 0, sizeof(*d));
    d->read = mem_read;
    d->write = mem_write;
    d->flush = mem_flush;
    d->ctx = m;
    d->size = sizeof(m->mem);
    d->block_size = RWY_BLOCK_SIZE;
}

static void set_ok(rwy_log_settings_t *s, rwy_log_setting_t id, const char *v) {
    const char *why = NULL;
    int rc = rwy_log_set(s, id, v, strlen(v), &why);
    if (rc) printf("parse failed: %s: %s\n", v, why ? why : "?");
    CHECK(rc == 0, "value parses");
}

static void test_layout(void) {
    CHECK(sizeof(rwy_log_block_t) <= RWY_BLOCK_SIZE, "settings block fits in a block");
    CHECK(sizeof(((rwy_log_settings_t *)0)->mac) == 6, "mac is six bytes");
    CHECK(rwy_log_setting_id("LOG-SERVER") == RWY_LOG_SERVER, "server id");
    CHECK(rwy_log_setting_id("LOG-INTERFACE") == RWY_LOG_INTERFACE, "interface id");
    CHECK(rwy_log_setting_id("LOG-ADDRESS") == RWY_LOG_ADDRESS, "address id");
    CHECK(rwy_log_setting_id("LOG-TIMEOUT") == RWY_LOG_TIMEOUT, "timeout id");
    CHECK(rwy_log_setting_id("LOG-BACKLOG") == RWY_LOG_BACKLOG, "backlog id");
    CHECK(rwy_log_setting_id("BOGUS") == -1, "unknown setting id");
}

static void test_defaults_and_parsing(void) {
    rwy_log_settings_t s;
    const char *why = NULL;
    char text[512];

    rwy_log_settings_init(&s);
    CHECK(s.flags == 0 && s.timeout == 5 && s.backlog == 8 && s.port == 514, "defaults");
    CHECK(rwy_log_settings_valid(&s, &why), "defaults are valid");

    set_ok(&s, RWY_LOG_SERVER, "192.0.2.10:5514");
    CHECK(s.server == 0xc000020a && s.port == 5514 && (s.flags & RWY_LOG_SET_SERVER), "server and port");
    set_ok(&s, RWY_LOG_INTERFACE, "02:11:22:33:44:55");
    CHECK(s.mac[0] == 2 && s.mac[5] == 0x55 && (s.flags & RWY_LOG_SET_MAC), "mac");
    set_ok(&s, RWY_LOG_ADDRESS, "198.51.100.7/24,198.51.100.1");
    CHECK(s.address == 0xc6336407 && s.prefix == 24 && s.gateway == 0xc6336401, "address and gateway");
    set_ok(&s, RWY_LOG_TIMEOUT, "30");
    set_ok(&s, RWY_LOG_BACKLOG, "0");
    CHECK(s.timeout == 30 && s.backlog == 0, "numeric settings");
    CHECK(rwy_log_settings_valid(&s, &why), "all settings valid");

    size_t text_len = rwy_log_format(&s, text, sizeof(text));
    text[text_len] = 0;
    CHECK(text_len == strlen(text) && !strcmp(text,
        "LOG-SERVER=192.0.2.10:5514\n"
        "LOG-INTERFACE=02:11:22:33:44:55\n"
        "LOG-ADDRESS=198.51.100.7/24,198.51.100.1\n"
        "LOG-TIMEOUT=30\n"
        "LOG-BACKLOG=0\n"), "format five lines");

    CHECK(rwy_log_set(&s, RWY_LOG_SERVER, "192.0.2.10:", 12, &why) == -1, "missing port");
    CHECK(rwy_log_set(&s, RWY_LOG_SERVER, "192.0.2.256", 11, &why) == -1, "bad server address");
    CHECK(rwy_log_set(&s, RWY_LOG_SERVER, "0.0.0.0", 7, &why) == -1, "zero server address");
    CHECK(rwy_log_set(&s, RWY_LOG_INTERFACE, "02:11:22:33:44", 14, &why) == -1, "short mac");
    CHECK(rwy_log_set(&s, RWY_LOG_INTERFACE, "01:11:22:33:44:55", 17, &why) == -1, "multicast mac");
    CHECK(rwy_log_set(&s, RWY_LOG_INTERFACE, "00:00:00:00:00:00", 17, &why) == -1, "zero mac");
    CHECK(rwy_log_set(&s, RWY_LOG_ADDRESS, "198.51.100.7", 12, &why) == -1, "address without prefix");
    CHECK(rwy_log_set(&s, RWY_LOG_ADDRESS, "198.51.100.7/33", 15, &why) == -1, "prefix 33");
    CHECK(rwy_log_set(&s, RWY_LOG_TIMEOUT, "0", 1, &why) == -1, "timeout 0");
    CHECK(rwy_log_set(&s, RWY_LOG_TIMEOUT, "31", 2, &why) == -1, "timeout 31");
    CHECK(rwy_log_set(&s, RWY_LOG_BACKLOG, "33", 2, &why) == -1, "backlog 33");

    rwy_log_unset(&s, RWY_LOG_TIMEOUT, &why);
    rwy_log_unset(&s, RWY_LOG_BACKLOG, &why);
    CHECK(s.timeout == 5 && s.backlog == 8, "unset restores defaults");
    rwy_log_unset(&s, RWY_LOG_SERVER, &why);
    CHECK(!(s.flags & RWY_LOG_SET_SERVER), "unset clears the flag");
    CHECK(rwy_log_settings_valid(&s, &why), "partial settings valid");
}

static void test_empty_and_ping_pong(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_log_info_t info;
    rwy_log_settings_t s, got;
    int rc;

    mem_init(&m, &d);
    rc = rwy_log_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info);
    CHECK(rc == RWY_ERR_CORRUPT, "empty area is corrupt to pick");
    CHECK(info.status[0] == RWY_LOG_EMPTY && info.status[1] == RWY_LOG_EMPTY, "both copies empty");

    rwy_log_settings_init(&s);
    set_ok(&s, RWY_LOG_SERVER, "203.0.113.9");
    CHECK(rwy_log_write(&d, BASE_BLOCK, COPY_BLOCKS, &s) == RWY_OK, "first write");
    CHECK(m.flushes == 1, "write flushes");
    CHECK(rwy_log_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK, "first pick");
    CHECK(info.copy == 0 && info.sequence == 1 && info.status[0] == RWY_LOG_OK, "copy A first");
    CHECK(info.settings.server == s.server && info.settings.port == 514, "server read back");

    s.timeout = 12;
    CHECK(rwy_log_write(&d, BASE_BLOCK, COPY_BLOCKS, &s) == RWY_OK, "second write");
    CHECK(rwy_log_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK, "second pick");
    CHECK(info.copy == 1 && info.sequence == 2 && info.status[0] == RWY_LOG_OK, "copy B second, A kept");
    CHECK(info.settings.timeout == 12, "timeout updated");

    s.backlog = 3;
    CHECK(rwy_log_write(&d, BASE_BLOCK, COPY_BLOCKS, &s) == RWY_OK, "third write");
    CHECK(rwy_log_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK, "third pick");
    CHECK(info.copy == 0 && info.sequence == 3, "copy A third");

    got = info.settings;
    CHECK(got.server == s.server && got.timeout == 12 && got.backlog == 3, "all values read back");
}

static void test_torn_and_bad_copy(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_log_info_t info;
    rwy_log_settings_t s;

    mem_init(&m, &d);
    rwy_log_settings_init(&s);
    set_ok(&s, RWY_LOG_SERVER, "203.0.113.10");
    CHECK(rwy_log_write(&d, BASE_BLOCK, COPY_BLOCKS, &s) == RWY_OK, "baseline write");

    s.timeout = 20;
    m.cut_after = m.writes;
    CHECK(rwy_log_write(&d, BASE_BLOCK, COPY_BLOCKS, &s) == RWY_ERR_IO, "torn write fails");
    CHECK(rwy_log_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK, "old copy still picked");
    CHECK(info.copy == 0 && info.sequence == 1 && info.settings.timeout == 5, "old values kept");

    m.cut_after = -1;
    memset(m.mem + (BASE_BLOCK + COPY_BLOCKS) * RWY_BLOCK_SIZE, 0x5a, RWY_BLOCK_SIZE);
    CHECK(rwy_log_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK, "bad copy B skipped");
    CHECK(info.status[1] == RWY_LOG_BAD && info.copy == 0, "copy B bad");
}

static void test_vread(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_log_settings_t s;
    char buf[128], all[512];
    size_t n, total;

    mem_init(&m, &d);
    rwy_log_settings_init(&s);
    set_ok(&s, RWY_LOG_TIMEOUT, "7");
    CHECK(rwy_log_write(&d, BASE_BLOCK, COPY_BLOCKS, &s) == RWY_OK, "view write");

    n = sizeof(all);
    CHECK(rwy_log_vread(&d, BASE_BLOCK, COPY_BLOCKS, 0, all, &n, &total) == RWY_OK, "full view");
    CHECK(n == total && !memcmp(all, "LOG-SERVER=-\nLOG-INTERFACE=-\nLOG-ADDRESS=-\nLOG-TIMEOUT=7\nLOG-BACKLOG=8\n", n),
          "view text");

    n = 4;
    CHECK(rwy_log_vread(&d, BASE_BLOCK, COPY_BLOCKS, 3, buf, &n, &total) == RWY_OK, "partial view");
    CHECK(n == 4 && !memcmp(buf, "-SER", 4), "partial view bytes");

    mem_init(&m, &d);
    n = sizeof(all);
    CHECK(rwy_log_vread(&d, BASE_BLOCK, COPY_BLOCKS, 0, all, &n, &total) == RWY_OK, "empty view");
    CHECK(!memcmp(all, "LOG-SERVER=-\nLOG-INTERFACE=-\nLOG-ADDRESS=-\nLOG-TIMEOUT=5\nLOG-BACKLOG=8\n", n),
          "empty view shows defaults");
}

int main(void) {
    test_layout();
    test_defaults_and_parsing();
    test_empty_and_ping_pong();
    test_torn_and_bad_copy();
    test_vread();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("netsettest: OK\n");
    return 0;
}

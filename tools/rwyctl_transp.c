/*
 * rwyctl_transp.c — `rwyctl flight show` and `rwyctl log show|verify`.
 * SPDX-License-Identifier: Apache-2.0
 * Flight recorder: plaintext ring buffer inspected without unlocking.
 * Update journal: encrypted audit ring buffer inspected after unlocking.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rwyctl.h"
#include "rwy_flight.h"
#include "rwy_transplog.h"
#include "rwy_internal.h"

#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_GREEN   "\033[1;32m"
#define COL_RED     "\033[1;31m"
#define COL_YELLOW  "\033[1;33m"
#define COL_CYAN    "\033[1;36m"
#define COL_MAGENTA "\033[1;35m"

static int use_color(void) {
    if (getenv("NO_COLOR")) return 0;
    return isatty(1);
}

static const char *flight_event_color(uint8_t ev, int col) {
    if (!col) return "";
    switch (ev) {
    case RWY_FLIGHT_EVENT_BOOT:         return COL_GREEN;
    case RWY_FLIGHT_EVENT_CONDEMN:      return COL_RED;
    case RWY_FLIGHT_EVENT_CONFIRM:      return COL_GREEN;
    case RWY_FLIGHT_EVENT_FAIL:         return COL_RED;
    case RWY_FLIGHT_EVENT_TRIES:        return COL_YELLOW;
    case RWY_FLIGHT_EVENT_RECOVER_NEXT: return COL_CYAN;
    case RWY_FLIGHT_EVENT_LOOP_LIMIT:   return COL_YELLOW;
    default:                             return "";
    }
}

static const char *log_event_color(uint8_t ev, int col) {
    if (!col) return "";
    switch (ev) {
    case RWY_JOURNAL_EVENT_CONFIRM: return COL_GREEN;
    case RWY_JOURNAL_EVENT_FAIL:    return COL_RED;
    case RWY_JOURNAL_EVENT_INSTALL: return COL_CYAN;
    case RWY_JOURNAL_EVENT_UPDATE:  return COL_YELLOW;
    case RWY_JOURNAL_EVENT_REMOVE:  return COL_MAGENTA;
    case RWY_JOURNAL_EVENT_EVICT:   return COL_MAGENTA;
    default:                         return "";
    }
}

static const char *flight_reason_str(uint8_t reason) {
    switch (reason) {
    case RWY_SS_REASON_NORMAL:        return "normal";
    case RWY_SS_REASON_REQUESTED:     return "requested";
    case RWY_SS_REASON_ALL_FAILED:    return "all_failed";
    case RWY_SS_REASON_INVALID_STORE: return "invalid_store";
    case RWY_SS_REASON_BOOT_LOOP:     return "boot_loop";
    default:                           return "-";
    }
}

static uint64_t read_max_rec_seq(const member_t *m) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint64_t best_seq = 0;
    for (unsigned c = 0; c < 2; c++) {
        if (m->raw.read(m->raw.ctx, (uint64_t)(m->rec_start + c) * RWY_BLOCK_SIZE, blk, sizeof(blk)) == 0) {
            const rwy_blk_header_t *h = (const rwy_blk_header_t *)blk;
            if (h->magic == RWY_BLK_MAGIC && h->kind == RWY_BLK_KIND_RECORD &&
                h->index == c && h->generation > 0 &&
                rwy_blk_compute_crc(blk) == h->crc32c) {
                if (h->generation > best_seq)
                    best_seq = h->generation;
            }
        }
    }
    return best_seq;
}

static const member_t *pick_flight_member(group_t *g) {
    for (int i = 0; i < g->n; i++) {
        if (g_mem[g->m[i]].ok) return &g_mem[g->m[i]];
    }
    return NULL;
}

static void print_flight_table_header(int col) {
    if (col) fputs(COL_BOLD, stdout);
    printf("%-6s %-12s %-12s %-14s %s\n",
           "SEQ", "EVENT", "SLOT", "REASON", "DETAILS");
    if (col) fputs(COL_RESET, stdout);
}

static void print_flight_entry_table(const rwy_flight_entry_t *e, int col) {
    char details[64];
    const char *ev_col = flight_event_color(e->event, col);
    const char *rst = col ? COL_RESET : "";
    const char *ev_name = rwy_flight_event_name(e->event);
    int off = snprintf(details, sizeof(details), "rec_seq=%llu", (unsigned long long)e->rec_seq);

    if (e->epoch > 0)
        off += snprintf(details + off, sizeof(details) > (size_t)off ? sizeof(details) - off : 0,
                        " epoch=%u", e->epoch);
    if (e->rev_hw > 0)
        snprintf(details + off, sizeof(details) > (size_t)off ? sizeof(details) - off : 0,
                 " rev_hw=%u", e->rev_hw);

    printf("%-6llu %s%-12s%s %-12s %-14s %s\n",
           (unsigned long long)e->seq,
           ev_col, ev_name, rst,
           e->booted[0] ? e->booted : "-",
           flight_reason_str(e->reason),
           details);
}

static void print_flight_entry_raw(const rwy_flight_entry_t *e) {
    char line[160];
    rwy_flight_format_entry(e, line, sizeof(line));
    fputs(line, stdout);
}

static int flight_show(group_t *g, int raw) {
    const member_t *m = pick_flight_member(g);
    rwy_flight_info_t info;
    uint64_t max_rec_seq;
    uint32_t count;
    int rc, col;

    if (!m) return X_IO;
    if (!m->flight_blocks) {
        warn("%s", "no flight recorder area");
        return X_POLICY;
    }

    rc = rwy_flight_pick(&m->raw, m->flight_start, m->flight_blocks, &info);
    if (rc) {
        if (info.status[0] == RWY_FLIGHT_EMPTY && info.status[1] == RWY_FLIGHT_EMPTY)
            return X_OK_;
        warn("%s", "flight recorder area corrupted");
        return X_CHECK;
    }

    max_rec_seq = read_max_rec_seq(m);
    count = rwy_flight_count_committed(&m->raw, m->flight_start, m->flight_blocks, &info, max_rec_seq);
    if (!count) return X_OK_;

    col = !raw && use_color();
    if (!raw) print_flight_table_header(col);

    for (int32_t i = (int32_t)count - 1; i >= 0; i--) {
        rwy_flight_entry_t e;
        if (rwy_flight_read_committed(&m->raw, m->flight_start, m->flight_blocks,
                                       &info, max_rec_seq, (uint32_t)i, &e) == RWY_OK) {
            if (raw) print_flight_entry_raw(&e);
            else print_flight_entry_table(&e, col);
        }
    }
    return X_OK_;
}

static int check_show_raw(int argc, char **argv, int *raw) {
    *raw = 0;
    if (argc < 1) return 0;
    if (!strcmp(argv[0], "show")) {
        if (argc >= 2 && !strcmp(argv[1], "--raw")) *raw = 1;
        return 1;
    }
    if (!strcmp(argv[0], "--raw") && argc >= 2 && !strcmp(argv[1], "show")) {
        *raw = 1;
        return 1;
    }
    return 0;
}

int cmd_flight(group_t *g, int argc, char **argv) {
    int raw = 0;
    if (check_show_raw(argc, argv, &raw))
        return flight_show(g, raw);
    warn("%s", "usage: flight show [--raw]");
    return X_USAGE;
}

static int is_all_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i]) return 0;
    }
    return 1;
}

static const char *evict_reason_name(uint8_t r) {
    return r == RWY_EVICT_REASON_QUOTA ? "quota" : r == RWY_EVICT_REASON_SLOTS ? "slots" :
           r == RWY_EVICT_REASON_INCOMPLETE ? "incomplete" : "?";
}

static void format_journal_details(const rwy_journal_entry_t *e, char *out, size_t sz) {
    int off = 0;

    if (!is_all_zero(e->payload_hash, 32)) {
        off += snprintf(out + off, sz > (size_t)off ? sz - off : 0,
                        "payload=%02x%02x%02x%02x.. ",
                        e->payload_hash[0], e->payload_hash[1],
                        e->payload_hash[2], e->payload_hash[3]);
    }
    off += snprintf(out + off, sz > (size_t)off ? sz - off : 0, "gen=%llu",
                    (unsigned long long)e->generation);
    if (e->rec_seq > 0)
        off += snprintf(out + off, sz > (size_t)off ? sz - off : 0, " rec_seq=%llu",
                        (unsigned long long)e->rec_seq);
    if (e->event == RWY_JOURNAL_EVENT_EVICT)
        off += snprintf(out + off, sz > (size_t)off ? sz - off : 0, " reason=%s blocks=%u",
                        evict_reason_name(e->reserved[RWY_EVICT_REASON_OFF]),
                        (unsigned)(e->reserved[RWY_EVICT_BLOCKS_OFF] | e->reserved[RWY_EVICT_BLOCKS_OFF + 1] << 8 |
                                   e->reserved[RWY_EVICT_BLOCKS_OFF + 2] << 16 | (unsigned)e->reserved[RWY_EVICT_BLOCKS_OFF + 3] << 24));
    if (e->epoch > 0)
        snprintf(out + off, sz > (size_t)off ? sz - off : 0, " epoch=%llu",
                 (unsigned long long)e->epoch);
}

static void print_journal_table_header(int col) {
    if (col) fputs(COL_BOLD, stdout);
    printf("%-6s %-9s %-9s %-5s %-20s %-8s %s\n",
           "SEQ", "EVENT", "SLOT", "REV", "LABEL", "CALLER", "DETAILS");
    if (col) fputs(COL_RESET, stdout);
}

static void print_journal_entry_table(const rwy_journal_entry_t *e, int col) {
    char rev[24], details[128];
    const char *ev_col = log_event_color(e->event, col);
    const char *rst = col ? COL_RESET : "";
    const char *ev_name = rwy_transplog_event_name(e->event);

    if (e->revision) snprintf(rev, sizeof(rev), "%llu", (unsigned long long)e->revision);
    else strcpy(rev, "-");

    format_journal_details(e, details, sizeof(details));

    printf("%-6llu %s%-9s%s %-9s %-5s %-20s %-8s %s\n",
           (unsigned long long)e->seq,
           ev_col, ev_name, rst,
           e->slot[0] ? e->slot : "-",
           rev,
           e->label[0] ? e->label : "-",
           e->caller[0] ? e->caller : "-",
           details);
}

static void print_journal_entry_raw(const rwy_journal_entry_t *e) {
    char ph[65];
    if (is_all_zero(e->payload_hash, 32)) {
        strcpy(ph, "-");
    } else {
        for (int k = 0; k < 32; k++)
            snprintf(ph + k * 2, 3, "%02x", e->payload_hash[k]);
    }
    printf("seq=%llu gen=%llu rec_seq=%llu event=%s slot=%s rev=%llu label=\"%s\" payload_hash=%s caller=%s epoch=%llu\n",
           (unsigned long long)e->seq,
           (unsigned long long)e->generation,
           (unsigned long long)e->rec_seq,
           rwy_transplog_event_name(e->event),
           e->slot[0] ? e->slot : "-",
           (unsigned long long)e->revision,
           e->label[0] ? e->label : "-",
           ph,
           e->caller[0] ? e->caller : "-",
           (unsigned long long)e->epoch);
}

static int log_show(int raw) {
    rwy_transplog_info_t info;
    uint32_t count;
    int rc, col;

    if (!g_store.sb.journal_blocks) {
        warn("%s", "no update journal area");
        return X_POLICY;
    }
    rc = rwy_transplog_pick(&g_store.dev, g_store.sb.journal_start, g_store.sb.journal_blocks, &info);
    if (rc) {
        if (info.status[0] == RWY_TRANSPLOG_EMPTY && info.status[1] == RWY_TRANSPLOG_EMPTY)
            return X_OK_;
        warn("%s", "update journal corrupted");
        return X_CHECK;
    }
    count = rwy_transplog_count_committed(&g_store.dev, g_store.sb.journal_start, g_store.sb.journal_blocks,
                                           &info, g_store.sb.generation, g_store.rec_seq);
    if (!count) return X_OK_;

    col = !raw && use_color();
    if (!raw) print_journal_table_header(col);

    for (int32_t i = (int32_t)count - 1; i >= 0; i--) {
        rwy_journal_entry_t e;
        if (rwy_transplog_read_committed(&g_store.dev, g_store.sb.journal_start, g_store.sb.journal_blocks,
                                          &info, g_store.sb.generation, g_store.rec_seq, (uint32_t)i, &e) == RWY_OK) {
            if (raw) print_journal_entry_raw(&e);
            else print_journal_entry_table(&e, col);
        }
    }
    return X_OK_;
}

/* The LAST-EVICT line/field of `status`: the newest committed eviction, or "-"/null. */
void print_last_evict(int json) {
    rwy_transplog_info_t info;
    uint32_t count = 0;
    int32_t i;
    rwy_journal_entry_t e;
    if (g_store.sb.journal_blocks &&
        !rwy_transplog_pick(&g_store.dev, g_store.sb.journal_start, g_store.sb.journal_blocks, &info))
        count = rwy_transplog_count_committed(&g_store.dev, g_store.sb.journal_start, g_store.sb.journal_blocks,
                                               &info, g_store.sb.generation, g_store.rec_seq);
    for (i = (int32_t)count - 1; i >= 0; i--) {
        if (rwy_transplog_read_committed(&g_store.dev, g_store.sb.journal_start, g_store.sb.journal_blocks,
                                          &info, g_store.sb.generation, g_store.rec_seq, (uint32_t)i, &e) != RWY_OK ||
            e.event != RWY_JOURNAL_EVENT_EVICT)
            continue;
        if (json) {
            printf("  \"last_evict\": {\"slot\": ");
            json_str(e.slot);
            printf(", \"reason\": \"%s\"},\n", evict_reason_name(e.reserved[RWY_EVICT_REASON_OFF]));
        } else {
            printf("LAST-EVICT=%s %s\n", e.slot, evict_reason_name(e.reserved[RWY_EVICT_REASON_OFF]));
        }
        return;
    }
    if (json) printf("  \"last_evict\": null,\n");
    else printf("LAST-EVICT=-\n");
}

static int log_verify(void) {
    rwy_transplog_info_t info;
    uint64_t max_epoch;
    int rc;

    if (!g_store.sb.journal_blocks) {
        warn("%s", "no update journal area");
        return X_POLICY;
    }
    rc = rwy_transplog_pick(&g_store.dev, g_store.sb.journal_start, g_store.sb.journal_blocks, &info);
    if (rc) {
        if (info.status[0] == RWY_TRANSPLOG_EMPTY && info.status[1] == RWY_TRANSPLOG_EMPTY) {
            printf("log verify: OK (0 entries)\n");
            return X_OK_;
        }
        warn("%s", "update journal corrupted");
        return X_CHECK;
    }
    if (info.count == 0) {
        printf("log verify: OK (0 entries)\n");
        return X_OK_;
    }
    max_epoch = rwy_ss_epoch(g_store.rec);
    rc = rwy_transplog_verify_chain(&g_store.dev, g_store.sb.journal_start, g_store.sb.journal_blocks,
                                     &info, max_epoch);
    if (rc == RWY_ERR_CORRUPT) {
        warn("%s", "journal hash chain broken or corrupted");
        return X_CHECK;
    }
    if (rc == RWY_ERR_RANGE) {
        warn("%s", "journal entry epoch exceeds store anchor epoch");
        return X_CHECK;
    }
    if (rc != RWY_OK) {
        warn("%s", "journal verification failed");
        return X_IO;
    }
    printf("log verify: OK (%u entries)\n", info.count);
    return X_OK_;
}

int cmd_log(int argc, char **argv) {
    int raw = 0;
    if (check_show_raw(argc, argv, &raw))
        return log_show(raw);
    if (argc >= 1 && !strcmp(argv[0], "verify"))
        return log_verify();
    warn("%s", "usage: log show [--raw] | log verify | log settings | log set NAME VALUE | log unset NAME");
    return X_USAGE;
}

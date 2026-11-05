/*
 * rwy_netreport.c — Send this boot's not-normal flight events to the syslog server.
 * SPDX-License-Identifier: Apache-2.0
 *
 * One UDP datagram per event, on the one interface the settings name, with the static address of the
 * settings. Every step runs inside LOG-TIMEOUT. A failure ends the report and nothing else.
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_netreport.h"
#include "rwy_flight.h"
#include "rwy_netset.h"

#define NET_STEP_MS 10u
#define NET_REQUIRED (RWY_LOG_SET_SERVER | RWY_LOG_SET_MAC | RWY_LOG_SET_ADDRESS)

typedef struct {
    UINT32 left_ms;
} budget_t;

/* Wait one step, polling the stack when udp is set. FALSE when the budget is spent. */
static BOOLEAN step(budget_t *b, EFI_UDP4 *udp) {
    if (b->left_ms < NET_STEP_MS) return FALSE;
    b->left_ms -= NET_STEP_MS;
    if (udp) udp->Poll(udp);
    gBS->Stall(NET_STEP_MS * 1000u);
    return TRUE;
}

static EFI_IPv4_ADDRESS ipv4_of(uint32_t v) {
    EFI_IPv4_ADDRESS a;
    a.Addr[0] = (UINT8)(v >> 24);
    a.Addr[1] = (UINT8)(v >> 16);
    a.Addr[2] = (UINT8)(v >> 8);
    a.Addr[3] = (UINT8)v;
    return a;
}

/* The handle with the UDP4 service binding whose permanent (else current) address is mac, or NULL.
 * The firmware puts the SNP on child handles too, so one NIC can show up several times. */
static EFI_HANDLE find_nic(const uint8_t mac[6]) {
    EFI_GUID snp_guid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID, sb_guid = EFI_UDP4_SERVICE_BINDING_PROTOCOL;
    EFI_HANDLE *handles, found = NULL;
    UINTN n;

    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &snp_guid, NULL, &n, &handles))) return NULL;
    for (UINTN i = 0; i < n && !found; i++) {
        EFI_SIMPLE_NETWORK *snp;
        void *sb;

        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &snp_guid, (void **)&snp))) continue;
        if (CompareMem(snp->Mode->PermanentAddress.Addr, mac, 6) &&
            CompareMem(snp->Mode->CurrentAddress.Addr, mac, 6)) continue;
        if (!EFI_ERROR(gBS->HandleProtocol(handles[i], &sb_guid, &sb))) found = handles[i];
    }
    gBS->FreePool(handles);
    return found;
}

/* The link can take seconds to come up: wait for it inside the budget. */
static BOOLEAN wait_link(EFI_HANDLE nic, budget_t *b) {
    EFI_GUID snp_guid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
    EFI_SIMPLE_NETWORK *snp;

    if (EFI_ERROR(gBS->HandleProtocol(nic, &snp_guid, (void **)&snp))) return FALSE;
    for (;;) {
        /* GetStatus refreshes MediaPresent; the stack does not do it for us. */
        (void)snp->GetStatus(snp, NULL, NULL);
        if (snp->Mode->MediaPresent) return TRUE;
        if (!step(b, NULL)) return FALSE;
    }
}

static EFI_STATUS udp4_child(EFI_HANDLE nic, EFI_SERVICE_BINDING **binding, EFI_HANDLE *child, EFI_UDP4 **udp) {
    EFI_GUID sb_guid = EFI_UDP4_SERVICE_BINDING_PROTOCOL, udp_guid = EFI_UDP4_PROTOCOL;
    EFI_STATUS st = gBS->HandleProtocol(nic, &sb_guid, (void **)binding);

    if (EFI_ERROR(st)) return st;
    st = (*binding)->CreateChild(*binding, child);
    if (EFI_ERROR(st)) return st;
    return gBS->HandleProtocol(*child, &udp_guid, (void **)udp);
}

/* Configure answers EFI_NO_MAPPING until the stack has the address: retry inside the budget. */
static EFI_STATUS udp4_setup(EFI_UDP4 *u, const rwy_log_settings_t *s, budget_t *b) {
    EFI_UDP4_CONFIG_DATA cfg;
    EFI_IPv4_ADDRESS any = ipv4_of(0), gw = ipv4_of(s->gateway);
    uint32_t mask = 0xffffffffu << (32 - s->prefix);
    EFI_STATUS st;

    memset(&cfg, 0, sizeof(cfg));
    cfg.StationAddress = ipv4_of(s->address);
    cfg.SubnetMask = ipv4_of(mask);
    cfg.RemoteAddress = ipv4_of(s->server);
    cfg.RemotePort = s->port;
    cfg.TimeToLive = 64;
    do {
        st = u->Configure(u, &cfg);
    } while ((st == EFI_NO_MAPPING || st == EFI_NOT_READY) && step(b, u));
    if (EFI_ERROR(st) || !s->gateway) return st;
    return u->Routes(u, FALSE, &any, &any, &gw);
}

/* Wait for the completion of a transmit. A token still pending at the end of the budget is cancelled. */
static EFI_STATUS wait_token(EFI_UDP4 *u, EFI_UDP4_COMPLETION_TOKEN *tok, budget_t *b) {
    while (gBS->CheckEvent(tok->Event) != EFI_SUCCESS) {
        if (!step(b, u)) {
            (void)u->Cancel(u, tok);
            return EFI_TIMEOUT;
        }
    }
    return tok->Status;
}

static EFI_STATUS udp4_send(EFI_UDP4 *u, const char *msg, size_t len, budget_t *b) {
    EFI_UDP4_TRANSMIT_DATA tx;
    EFI_UDP4_COMPLETION_TOKEN tok;
    EFI_STATUS st;

    memset(&tx, 0, sizeof(tx));
    memset(&tok, 0, sizeof(tok));
    st = gBS->CreateEvent(0, 0, NULL, NULL, &tok.Event);
    if (EFI_ERROR(st)) return st;
    tx.DataLength = (UINT32)len;
    tx.FragmentCount = 1;
    tx.FragmentTable[0].FragmentLength = (UINT32)len;
    tx.FragmentTable[0].FragmentBuffer = (void *)msg;
    tok.Packet.TxData = &tx;
    st = u->Transmit(u, &tok);
    if (!EFI_ERROR(st)) st = wait_token(u, &tok, b);
    gBS->CloseEvent(tok.Event);
    return st;
}

static char *put_num(char *p, unsigned v, unsigned digits) {
    for (unsigned d = digits; d--; v /= 10) p[d] = (char)('0' + v % 10);
    return p + digits;
}

/* RFC 5424 TIMESTAMP of the clock, or "-" when the clock or its time zone is unknown. */
static void clock_stamp(char out[32]) {
    EFI_TIME t;
    char *p = out;
    int off;

    if (EFI_ERROR(gRT->GetTime(&t, NULL)) || t.TimeZone == EFI_UNSPECIFIED_TIMEZONE) {
        out[0] = '-';
        out[1] = '\0';
        return;
    }
    p = put_num(p, t.Year, 4);
    *p++ = '-';
    p = put_num(p, t.Month, 2);
    *p++ = '-';
    p = put_num(p, t.Day, 2);
    *p++ = 'T';
    p = put_num(p, t.Hour, 2);
    *p++ = ':';
    p = put_num(p, t.Minute, 2);
    *p++ = ':';
    p = put_num(p, t.Second, 2);
    /* EFI_TIME counts minutes west of UTC; RFC 5424 wants the offset east of UTC. */
    off = -(int)t.TimeZone;
    if (off == 0) {
        *p++ = 'Z';
    } else {
        unsigned a = (unsigned)(off < 0 ? -off : off);
        *p++ = off < 0 ? '-' : '+';
        p = put_num(p, a / 60, 2);
        *p++ = ':';
        p = put_num(p, a % 60, 2);
    }
    *p = '\0';
}

/* One datagram for each entry that has a line, in order. Stops at the first failed send. */
static EFI_STATUS send_entries(EFI_UDP4 *u, const rwy_flight_entry_t *e, uint32_t n, const char *stamp,
                               const char *host, budget_t *b, unsigned *sent) {
    char line[RWY_FLIGHT_SYSLOG_MAX];
    EFI_STATUS st = EFI_SUCCESS;

    for (uint32_t i = 0; i < n && !EFI_ERROR(st); i++) {
        size_t len = rwy_flight_format_syslog(&e[i], stamp, host, line);

        if (!len) continue;
        st = udp4_send(u, line, len, b);
        if (!EFI_ERROR(st)) (*sent)++;
    }
    return st;
}

/* True when a ring entry is one of this boot's events. The ring adds only the sequence number. */
static BOOLEAN is_boot_event(const rwy_flight_entry_t *e, const rwy_flight_entry_t *boot, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const rwy_flight_entry_t *b = &boot[i];

        if (e->generation == b->generation && e->rec_seq == b->rec_seq && e->timestamp == b->timestamp &&
            e->event == b->event && e->reason == b->reason && e->epoch == b->epoch && e->rev_hw == b->rev_hw &&
            CompareMem(e->booted, b->booted, sizeof(e->booted)) == 0)
            return TRUE;
    }
    return FALSE;
}

/* Drop the ring entries that are this boot's events, which are sent first. Returns the new count. */
static uint32_t drop_boot_events(rwy_flight_entry_t *t, uint32_t n, const rwy_flight_entry_t *boot, uint32_t nb) {
    uint32_t kept = 0;

    for (uint32_t i = 0; i < n; i++) {
        if (!is_boot_event(&t[i], boot, nb)) t[kept++] = t[i];
    }
    return kept;
}

/* The settings of a member when they are complete; FALSE when the feature is off or they cannot be read. */
static BOOLEAN settings_of(const rwy_block_dev_t *dev, const rwy_superblock_t *sb, rwy_log_settings_t *out) {
    rwy_log_info_t info;

    if (!sb->net_blocks) return FALSE;
    if (rwy_log_pick(dev, sb->net_start, sb->net_blocks, &info) != RWY_OK) return FALSE;
    if ((info.settings.flags & NET_REQUIRED) != NET_REQUIRED) return FALSE;
    *out = info.settings;
    return TRUE;
}

/* Send the boot events, then the backlog of the ring when ring is set. */
static void send_report(const rwy_log_settings_t *c, const rwy_superblock_t *sb, const rwy_block_dev_t *ring,
                        uint64_t rec_seq, const rwy_flight_entry_t *boot, uint32_t nb) {
    EFI_SERVICE_BINDING *binding = NULL;
    EFI_HANDLE nic, child = NULL;
    EFI_UDP4 *udp = NULL;
    budget_t b;
    char host[37], stamp[32];
    rwy_flight_entry_t tail[RWY_LOG_BACKLOG_MAX];
    uint32_t n;
    unsigned sent = 0;
    EFI_STATUS st = EFI_SUCCESS;

    b.left_ms = (UINT32)c->timeout * 1000u;
    nic = find_nic(c->mac);
    if (!nic) {
        RWY_WARN("report: no interface\n");
        return;
    }
    if (!wait_link(nic, &b)) {
        RWY_WARN("report: no link inside the budget\n");
        return;
    }
    st = udp4_child(nic, &binding, &child, &udp);
    if (!EFI_ERROR(st)) st = udp4_setup(udp, c, &b);
    if (!EFI_ERROR(st)) {
        rwy_format_uuid(sb->uuid, host);
        clock_stamp(stamp);
        st = send_entries(udp, boot, nb, stamp, host, &b, &sent);
        if (ring && !EFI_ERROR(st)) {
            n = rwy_flight_tail(ring, sb->flight_start, sb->flight_blocks, rec_seq, c->backlog, tail);
            n = drop_boot_events(tail, n, boot, nb);
            st = send_entries(udp, tail, n, stamp, host, &b, &sent);
        }
    }
    if (child) binding->DestroyChild(binding, child);
    if (EFI_ERROR(st)) RWY_WARN("report: stopped after %u sent (0x%llx)\n", sent, (unsigned long long)st);
    else RWY_INFO("report: %u sent\n", sent);
}

void rwy_netreport_send(rwy_store_t *s) {
    rwy_log_settings_t c;

    if (!s->flight_boot_count) return;
    if (!settings_of(&s->dev, &s->sb, &c)) return;
    send_report(&c, &s->sb, &s->dev, s->rec_seq, s->flight_boot, s->flight_boot_count);
}

void rwy_netreport_send_unmounted(const rwy_block_dev_t devs[], const rwy_superblock_t sbs[], UINTN count) {
    rwy_flight_entry_t store_event;
    rwy_log_settings_t c;

    for (UINTN i = 0; i < count; i++) {
        if (!settings_of(&devs[i], &sbs[i], &c)) continue;
        memset(&store_event, 0, sizeof(store_event));
        store_event.event = RWY_FLIGHT_EVENT_STORE;
        send_report(&c, &sbs[i], NULL, 0, &store_event, 1);
        return;
    }
}

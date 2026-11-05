/* rwyctl: the mirror health counters and member roster of the state record: the `status` section, `mirror clear` and `mirror forget`. */
/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>

#include "rwyctl.h"

/* How the member looks now: "online", "lagging" (left out of this mount) or "absent". */
static const char *member_state(const uint8_t uuid[16]) {
    for (size_t i = 0; i < g_mirror.num_devs; i++) {
        uint8_t u[16];
        if (rwy_block_member_uuid(&g_mirror.devs[i], u) && !memcmp(u, uuid, 16))
            return g_mirror.dev_faulted[i] ? "lagging" : "online";
    }
    return "absent";
}

static void print_member(const uint8_t uuid[16], const rwy_ss_health_t *h) {
    char text[40];
    rwy_format_uuid(uuid, text);
    printf("MIRROR-MEMBER=%s STATE=%s LAGGING=%u SYNC_FAILED=%u WRITE_FAILED=%u MISSING=%u LAST=%s@%llu\n", text,
           member_state(uuid), h->count[RWY_SS_EV_LAGGING], h->count[RWY_SS_EV_SYNC_FAILED],
           h->count[RWY_SS_EV_WRITE_FAILED], h->count[RWY_SS_EV_MISSING], rwy_ss_event_name(h->last_event),
           (unsigned long long)h->seq);
}

void print_mirror_health(void) {
    uint8_t r[RWY_REC_PAYLOAD], uuid[16];
    rwy_ss_health_t h;
    uint32_t resets;
    if (rwy_record_read(&g_store, r)) return;
    resets = rwy_ss_record_reset(r);
    for (unsigned i = 0; i < RWY_MIRROR_MAX_DEVS; i++)
        if (rwy_ss_health_at(r, i, uuid, &h)) print_member(uuid, &h);
    for (unsigned i = 0; i < RWY_MIRROR_MAX_DEVS; i++) {   /* a roster member with no counter yet */
        if (!rwy_ss_roster_at(r, i, uuid) || rwy_ss_health_get(r, uuid, &h)) continue;
        memset(&h, 0, sizeof(h));
        print_member(uuid, &h);
    }
    if (resets) printf("RECORD-RESETS=%u\n", resets);
}

/* mirror clear drops the counters after a repair; mirror forget UUID drops a member that is gone for good. Both need
 * a store the user named, as the SYSTEM actor. */
int cmd_mirror(int argc, char **argv) {
    uint8_t uuid[16];
    int rc, forget = argc == 2 && !strcmp(argv[0], "forget");
    if (!forget && (argc != 1 || strcmp(argv[0], "clear"))) { warn("%s", "usage: mirror clear | mirror forget UUID"); return X_USAGE; }
    if (forget && rwy_parse_uuid(argv[1], uuid)) { warn("bad UUID %s", argv[1]); return X_USAGE; }
    if ((rc = system_allowed())) return rc;
    rc = forget ? rwy_record_forget_member(&g_store, uuid) : rwy_record_clear_health(&g_store);
    if (rc == RWY_ERR_NOENT) { warn("%s", "that member is not in the roster"); return X_USAGE; }
    if (rc) { warn("cannot write the record: %s", rwy_strerror(rc)); return X_IO; }
    return 0;
}

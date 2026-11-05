/*
 * rwy_guardvar.c — The boot loop guard counter in the RunwayBootGuard NV variable.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include "rwy_boot.h"
#include "rwy_loopguard.h"

#define RWY_GUARD_VAR_GUID { 0xb371a076, 0xb7ef, 0x4e6e, { 0xb9, 0x8a, 0x8d, 0x0d, 0x2c, 0x49, 0x3c, 0x7b } }

/* Seconds in UTC, or 0 when the clock cannot be read. */
static uint64_t guard_now(void) {
    EFI_TIME t;
    uint64_t s;
    if (EFI_ERROR(gST->RuntimeServices->GetTime(&t, NULL))) return 0;
    s = rwy_guard_secs(t.Year, t.Month, t.Day, t.Hour, t.Minute, t.Second);
    if (s && t.TimeZone != EFI_UNSPECIFIED_TIMEZONE) s -= (uint64_t)((int64_t)t.TimeZone * 60);
    return s;
}

void guard_clear(void) {
    EFI_GUID guid = RWY_GUARD_VAR_GUID;
    EFI_STATUS s = gST->RuntimeServices->SetVariable(L"RunwayBootGuard", &guid, 0, 0, NULL);
    if (EFI_ERROR(s) && s != EFI_NOT_FOUND) RWY_WARN("cannot clear the boot loop guard counter\n");
}

/* Count an entry into the OK slot `revision`. Anything wrong with the variable or the clock means no
 * count and RWY_GUARD_BOOT: the guard only ever acts on what it could read and write back. */
int guard_check(uint64_t revision, const rwy_loop_guard_t *g) {
    EFI_GUID guid = RWY_GUARD_VAR_GUID;
    uint8_t b[RWY_GUARD_VAR_LEN + 1];
    UINTN size = sizeof(b);
    rwy_guard_var_t old, next;
    int have, verdict;

    have = !EFI_ERROR(gST->RuntimeServices->GetVariable(L"RunwayBootGuard", &guid, NULL, &size, b)) &&
           rwy_guard_var_decode(b, size, &old);
    verdict = rwy_guard_count(have ? &old : NULL, revision, guard_now(), g, &next);
    if (verdict != RWY_GUARD_BOOT_WRITE) return verdict;
    rwy_guard_var_encode(&next, b);
    if (EFI_ERROR(gST->RuntimeServices->SetVariable(L"RunwayBootGuard", &guid,
            EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            RWY_GUARD_VAR_LEN, b)))
        RWY_WARN("cannot write the boot loop guard counter\n");
    return RWY_GUARD_BOOT;
}

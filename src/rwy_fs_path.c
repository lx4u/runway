/*
 * rwy_fs_path.c — Path and time conversion between EFI and the store.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_fs_internal.h"
#include "efi_util.h"

/* Copy a string into a fixed buffer, cutting it when too long. */
void copy_str(char *dst, UINTN size, const char *src) {
    UINTN i = 0;
    while (src[i] && i + 1 < size) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* CHAR16 UEFI path -> ASCII store path. */
EFI_STATUS c16_to_store(const CHAR16 *w, char *out, UINTN outsz) {
    UINTN n = 0;
    while (w[n]) n++;
    if (n + 1 > outsz) return EFI_BAD_BUFFER_SIZE;
    for (UINTN i = 0; i <= n; i++) {
        CHAR16 c = w[i];
        if (c == (CHAR16)'\\') out[i] = '/';
        else if (c < 0x20 || c > 0x7e) {
            if (c != 0) return EFI_INVALID_PARAMETER; /* ASCII only */
            out[i] = 0;
        } else out[i] = (char)c;
    }
    return EFI_SUCCESS;
}

/* Add one path component to the normalized path in tmp: "." stays, ".." drops the last one. */
static EFI_STATUS path_component(char *tmp, UINTN *tl, UINTN size, const char *p, UINTN cl) {
    if (cl == 1 && p[0] == '.') return EFI_SUCCESS;
    if (cl == 2 && p[0] == '.' && p[1] == '.') {
        while (*tl && tmp[*tl - 1] != '/') (*tl)--;
        if (*tl) (*tl)--;
        return EFI_SUCCESS;
    }
    if (*tl) {
        if (*tl + 1 >= size) return EFI_BAD_BUFFER_SIZE;
        tmp[(*tl)++] = '/';
    }
    if (*tl + cl >= size) return EFI_BAD_BUFFER_SIZE;
    for (UINTN i = 0; i < cl; i++) tmp[(*tl)++] = p[i];
    return EFI_SUCCESS;
}

/* Resolve `name` against dir `base`. Result: normalized path ("" = root). */
EFI_STATUS resolve_path(const char *base, const char *name,
                               char *out, UINTN outsz) {
    char tmp[RWY_MAX_PATH];
    UINTN tl = 0;
    const char *p;
    EFI_STATUS s;
    int absolute = (name[0] == '/');
    if (!absolute && base[0]) {
        for (p = base; *p && tl + 1 < sizeof(tmp); p++) tmp[tl++] = *p;
    }
    p = name;
    while (*p) {
        const char *e;
        while (*p == '/') p++;
        if (!*p) break;
        e = p;
        while (*e && *e != '/') e++;
        s = path_component(tmp, &tl, sizeof(tmp), p, (UINTN)(e - p));
        if (EFI_ERROR(s)) return s;
        p = e;
    }
    tmp[tl] = 0;
    if (tl + 1 > outsz) return EFI_BAD_BUFFER_SIZE;
    for (UINTN i = 0; i <= tl; i++) out[i] = tmp[i];
    return EFI_SUCCESS;
}

void ascii_to_c16(const char *a, CHAR16 *w, UINTN wcap) {
    UINTN i = 0;
    while (a[i] && i + 1 < wcap) { w[i] = (CHAR16)(unsigned char)a[i]; i++; }
    w[i] = 0;
}

/* Unix seconds -> EFI_TIME (civil-from-days; valid for 1970..2106). */
void unix_to_efitime(INT64 secs, EFI_TIME *t) {
    INT64 days = secs / 86400;
    INT64 rem = secs % 86400;
    INT64 z, era, doe, yoe, y, doy, mp, d, m;
    if (rem < 0) { rem += 86400; days--; }
    z = days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    t->Year = (UINT16)y; t->Month = (UINT8)m; t->Day = (UINT8)d;
    t->Hour = (UINT8)(rem / 3600);
    t->Minute = (UINT8)((rem % 3600) / 60);
    t->Second = (UINT8)(rem % 60);
    t->Nanosecond = 0;
    t->TimeZone = 0;
    t->Daylight = 0;
    t->Pad1 = 0; t->Pad2 = 0;
}

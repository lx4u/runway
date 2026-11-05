/* SPDX-License-Identifier: Apache-2.0 */
#include "efi_util.h"
#include <efistdarg.h>

/* libefi's image-handle global (see lib.h). Not via headers: those pull in
 * the EFI_FUNCTION_WRAPPER prototypes, which we deliberately avoid. */
extern EFI_HANDLE LibImageHandle;

/* The freestanding strlen below satisfies GCC-synthesized calls (loop
 * idioms). No libc exists in EFI, so an unresolved U reference would be
 * a load-time/crash-time bomb. memset/memcpy already come from libefi.a;
 * strlen does not, hence only it is defined here. */
EFI_HANDLE gImageHandle;

typedef unsigned long size_t_;
size_t_ strlen(const char *s) {
    size_t_ n = 0;
    while (s[n]) n++;
    return n;
}

static EFI_SERIAL_IO_PROTOCOL *gSerial;
static int gConOutHasSerial; /* ConOut already reaches the serial port */
static UINT64 gTicksPerSec; /* TSC calibration */
static UINT64 rdtsc_raw(void);
#if defined(__aarch64__)
static UINT64 cntfrq_raw(void);
#endif

/* ---------- minimal string/convert helpers (freestanding) ---------- */

static void out_conout(const char *s, UINTN len) {
    /* Widen ASCII -> CHAR16 on the stack; len is at most 255. */
    CHAR16 w[256];
    UINTN i;
    if (!gST || !gST->ConOut) return;
    for (i = 0; i < len; i++) w[i] = (CHAR16)(unsigned char)s[i];
    w[len] = 0;
    gST->ConOut->OutputString(gST->ConOut, w);
}

static void out_serial(const char *s, UINTN len) {
    if (!gSerial || gConOutHasSerial) return;
    /* SerialIo Write is best-effort; ignore errors (ConOut is primary). */
    gSerial->Write(gSerial, &len, (VOID *)s);
}

static void emit_u64(char *buf, UINTN *pos, UINT64 v, int base, int upper) {
    char tmp[32];
    int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = dig[v % (UINT64)base]; v /= (UINT64)base; }
    while (n--) buf[(*pos)++] = tmp[n];
}

static void emit_i64(char *buf, UINTN *pos, INT64 v) {
    if (v < 0) { buf[(*pos)++] = '-'; v = -v; }
    emit_u64(buf, pos, (UINT64)v, 10, 0);
}

typedef struct {
    int left, zeropad, width, longcount;
} spec_t;

/* Parse flags, width, precision (ignored) and length modifiers after the '%'. */
static const char *parse_spec(const char *fmt, spec_t *sp) {
    sp->left = sp->zeropad = sp->width = sp->longcount = 0;
    while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' || *fmt == '0') {
        if (*fmt == '-') sp->left = 1;
        if (*fmt == '0') sp->zeropad = 1;
        fmt++;
    }
    while (*fmt >= '0' && *fmt <= '9') { sp->width = sp->width * 10 + (*fmt - '0'); fmt++; }
    if (*fmt == '.') {
        fmt++;
        while (*fmt >= '0' && *fmt <= '9') fmt++;
    }
    while (*fmt == 'l') { sp->longcount++; fmt++; }
    if (*fmt == 'z') fmt++;
    return fmt;
}

static unsigned long long arg_unsigned(va_list *ap, int longcount) {
    if (longcount >= 2) return va_arg(*ap, unsigned long long);
    if (longcount == 1) return (unsigned long long)va_arg(*ap, unsigned long);
    return (unsigned long long)va_arg(*ap, unsigned int);
}

static long long arg_signed(va_list *ap, int longcount) {
    if (longcount >= 2) return va_arg(*ap, long long);
    if (longcount == 1) return (long long)va_arg(*ap, long);
    return (long long)va_arg(*ap, int);
}

/* Render one conversion into nb (or point *str at the argument). Returns 0 for an unknown one. */
static int convert(char conv, va_list *ap, const spec_t *sp, char *nb, const char **str, UINTN *slen) {
    UINTN np = 0;

    *str = nb;
    switch (conv) {
    case 's': {
        const char *s = va_arg(*ap, const char *);
        UINTN n = 0;
        if (!s) s = "(null)";
        while (s[n] && n < 256) n++;
        *str = s;
        *slen = n;
        return 1;
    }
    case 'c': nb[np++] = (char)va_arg(*ap, int); break;
    case 'd': case 'i': emit_i64(nb, &np, (INT64)arg_signed(ap, sp->longcount)); break;
    case 'u': emit_u64(nb, &np, (UINT64)arg_unsigned(ap, sp->longcount), 10, 0); break;
    case 'x': case 'X': emit_u64(nb, &np, (UINT64)arg_unsigned(ap, sp->longcount), 16, conv == 'X'); break;
    case 'p':
        nb[np++] = '0';
        nb[np++] = 'x';
        emit_u64(nb, &np, (UINT64)(UINTN)va_arg(*ap, void *), 16, 0);
        break;
    default:
        return 0;
    }
    *slen = np;
    return 1;
}

/* mini vsnprintf: %s %c %d %i %u %x %X %p %% with ll/z length mods,
 * '-' flag and decimal width (others ignored). Width pads with spaces
 * (left-justify with '-', else right-justify). Unknown specs echo '%'. */
static void mini_vsnprintf(char *out, UINTN outsz, const char *fmt, va_list ap) {
    UINTN pos = 0;
    va_list args;
    va_copy(args, ap);   /* a va_list parameter cannot be passed on by address */
#define PUT(c) do { if (pos + 1 < outsz) out[pos++] = (c); } while (0)
#define PUTN(s, n) do { for (UINTN _i = 0; _i < (n) && pos + 1 < outsz; _i++) out[pos++] = (s)[_i]; } while (0)
    while (*fmt && pos + 1 < outsz) {
        spec_t sp;
        char nb[64];
        const char *str;
        UINTN slen = 0;
        int width;
        char padc;
        if (*fmt != '%') { PUT(*fmt++); continue; }
        fmt++;
        if (*fmt == '%') { PUT('%'); fmt++; continue; }
        fmt = parse_spec(fmt, &sp);
        if (!convert(*fmt, &args, &sp, nb, &str, &slen)) {
            PUT('%'); /* unknown spec: echo; do not consume arg */
            continue;
        }
        fmt++;
        width = sp.width;
        padc = (sp.zeropad && !sp.left) ? '0' : ' ';
        if (!sp.left)
            while ((int)slen < width && pos + 1 < outsz) { out[pos++] = padc; width--; }
        PUTN(str, slen);
        if (sp.left)
            while ((int)slen < width && pos + 1 < outsz) { out[pos++] = ' '; width--; }
    }
    out[pos] = '\0';
    va_end(args);
#undef PUT
#undef PUTN
}

/* True when a UART node is in the console output device paths. */
static int conout_has_uart(void) {
    EFI_GUID gv = EFI_GLOBAL_VARIABLE;
    CHAR16 name[] = { 'C', 'o', 'n', 'O', 'u', 't', 0 };
    UINT8 buf[1024];
    UINTN size = sizeof(buf), off = 0;
    if (!gST || !gST->RuntimeServices) return 0;
    if (EFI_ERROR(gST->RuntimeServices->GetVariable(name, &gv, NULL, &size, buf))) return 0;
    while (off + sizeof(EFI_DEVICE_PATH) <= size) {
        EFI_DEVICE_PATH *n = (EFI_DEVICE_PATH *)(buf + off);
        UINTN len = (UINTN)n->Length[0] | ((UINTN)n->Length[1] << 8);
        if (len < sizeof(EFI_DEVICE_PATH) || off + len > size) break;
        if (n->Type == MESSAGING_DEVICE_PATH && n->SubType == MSG_UART_DP) return 1;
        off += len;
    }
    return 0;
}

/* ---------- public API ---------- */

void util_init(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st) {
    EFI_STATUS s;
    EFI_GUID serial_guid = EFI_SERIAL_IO_PROTOCOL_GUID;
    /* Manual equivalent of InitializeLib: our TUs are ms_abi
     * (HAVE_USE_MS_ABI) but libefi's InitializeLib is a SysV
     * EFI_FUNCTION_WRAPPER build — calling it would swap our arguments.
     * ST/BS/LibImageHandle live in libefi's .bss; just set them. */
    ST = st;
    BS = st->BootServices;
    LibImageHandle = ih;
    gImageHandle = ih;
    gSerial = NULL;
    /* Best-effort serial attach (COM1). Ignore failures. */
    s = gBS->LocateProtocol(&serial_guid, NULL, (VOID **)&gSerial);
    if (EFI_ERROR(s)) gSerial = NULL;
    gConOutHasSerial = conout_has_uart();
    if (!gTicksPerSec) {
#if defined(__aarch64__)
        UINT64 frq = cntfrq_raw();
        gTicksPerSec = frq ? frq : 1000000000ULL;
#else
        if (gBS) {
            UINT64 a = rdtsc_raw();
            gBS->Stall(10000);
            UINT64 b = rdtsc_raw();
            gTicksPerSec = (b > a && b - a > 1000) ? ((b - a) * 100ULL) : 3000000000ULL;
        }
#endif
    }
}

static char *gCapture;
static UINTN gCaptureCap, gCaptureLen;

void uprint_capture(char *buf, UINTN cap) {
    gCapture = buf;
    gCaptureCap = cap;
    gCaptureLen = 0;
}

UINTN uprint_captured(const char **text) {
    *text = gCapture;
    return gCaptureLen;
}

/* Append to the capture; when it is full, drop the older half. */
static void capture_text(const char *s, UINTN len) {
    if (!gCapture || len >= gCaptureCap) return;
    if (gCaptureLen + len > gCaptureCap) {
        UINTN keep = gCaptureCap / 2;

        if (keep > gCaptureLen) keep = gCaptureLen;
        for (UINTN i = 0; i < keep; i++) gCapture[i] = gCapture[gCaptureLen - keep + i];
        gCaptureLen = keep;
    }
    for (UINTN i = 0; i < len; i++) gCapture[gCaptureLen + i] = s[i];
    gCaptureLen += len;
}

/* Send text to the screen and the serial port. Firmware consoles need
 * CR LF, so an LF without a CR before it gets one. */
void uprint(const char *s) {
    UINTN len = 0;
    char buf[256];
    UINTN n = 0;
    char prev = 0;
    while (s[len]) len++;
    capture_text(s, len);
    for (;; s++) {
        if (n && (*s == '\0' || n >= sizeof(buf) - 2)) {
            out_conout(buf, n);
            out_serial(buf, n);
            n = 0;
        }
        if (*s == '\0') break;
        if (*s == '\n' && prev != '\r') buf[n++] = '\r';
        buf[n++] = *s;
        prev = *s;
    }
}

void uprint_color(UINTN attr, const char *s) {
    SIMPLE_TEXT_OUTPUT_INTERFACE *co = gST ? gST->ConOut : NULL;
    INT32 saved;

    if (!co || !co->Mode) { uprint(s); return; }
    saved = co->Mode->Attribute;
    co->SetAttribute(co, (attr & 0x0F) | ((UINTN)saved & 0x70));
    uprint(s);
    co->SetAttribute(co, (UINTN)saved);
}

void uprintf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    mini_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    uprint(buf);
}

static void (*gFailHook)(void);

void ufail_hook(void (*fn)(void)) {
    gFailHook = fn;
}

EFI_STATUS ufail(const char *msg, EFI_STATUS st) {
    uprintf("FAIL: %s (status 0x%llx)\n", msg, (unsigned long long)st);
    if (gFailHook) gFailHook();
    flush_and_stall(500000);
    if (gST && gST->RuntimeServices && gST->RuntimeServices->ResetSystem) {
        gST->RuntimeServices->ResetSystem(EfiResetShutdown, st, 0, NULL);
    }
    return st;
}

void flush_and_stall(UINTN microseconds) {
    if (gBS) gBS->Stall((UINTN)microseconds);
}

EFI_STATUS fat_open_our_root(EFI_HANDLE ih, EFI_FILE_HANDLE *out_root) {
    EFI_STATUS s;
    EFI_LOADED_IMAGE *li;
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
    s = gBS->HandleProtocol(ih, &li_guid, (VOID **)&li);
    if (EFI_ERROR(s)) return s;
    s = gBS->HandleProtocol(li->DeviceHandle, &sfs_guid, (VOID **)&sfs);
    if (EFI_ERROR(s)) return s;
    return sfs->OpenVolume(sfs, out_root);
}

EFI_STATUS fat_read_file(EFI_FILE_HANDLE root, const CHAR16 *path,
                         UINT8 **out_buf, UINTN *out_size) {
    EFI_STATUS s;
    EFI_FILE_HANDLE fh = NULL;
    UINT64 fsize = 0;
    UINT8 info_buf[256];
    UINTN info_sz = sizeof(info_buf);
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info_buf;
    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    UINT8 *buf = NULL;
    UINTN left, got;
    UINT64 pos = 0;

    s = root->Open(root, &fh, (CHAR16 *)path,
                   EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s)) return s;
    s = fh->GetInfo(fh, &info_guid, &info_sz, fi);
    if (EFI_ERROR(s)) { fh->Close(fh); return s; }
    fsize = fi->FileSize;
    if (fsize > (UINT64)(512 * 1024 * 1024)) { fh->Close(fh); return EFI_BAD_BUFFER_SIZE; }
    s = gBS->AllocatePool(EfiLoaderData, (UINTN)fsize + 1, (VOID **)&buf);
    if (EFI_ERROR(s)) { fh->Close(fh); return s; }
    left = (UINTN)fsize;
    while (left) {
        got = left > 1048576 ? 1048576 : left;
        s = fh->Read(fh, &got, buf + pos);
        if (EFI_ERROR(s) || got == 0) break;
        pos += got;
        left -= got;
    }
    fh->Close(fh);
    if (EFI_ERROR(s)) { gBS->FreePool(buf); return s; }
    buf[fsize] = 0;
    *out_buf = buf;
    *out_size = (UINTN)fsize;
    return EFI_SUCCESS;
}

/* ---------- TSC ---------- */

#if defined(__x86_64__)
static UINT64 rdtsc_raw(void) {
    UINT32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((UINT64)hi << 32) | lo;
}
#elif defined(__aarch64__)
static UINT64 rdtsc_raw(void) {
    UINT64 val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}
static UINT64 cntfrq_raw(void) {
    UINT64 frq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    return frq;
}
#else
static UINT64 rdtsc_raw(void) { return 0; }
#endif

void tsc_calibrate(void) {
#if defined(__aarch64__)
    UINT64 frq = cntfrq_raw();
    gTicksPerSec = frq ? frq : 1000000000ULL;
#else
    UINT64 a, b;
    if (!gBS) { gTicksPerSec = 3000000000ULL; return; }
    a = rdtsc_raw();
    gBS->Stall(1000000); /* 1 s */
    b = rdtsc_raw();
    gTicksPerSec = (b > a && b - a > 1000) ? (b - a) : 3000000000ULL;
#endif
}

UINT64 tsc_now(void) { return rdtsc_raw(); }

UINT64 tsc_hz(void) { return gTicksPerSec; }

double tsc_to_seconds(UINT64 d) {
    if (!gTicksPerSec) return 0.0;
    return (double)d / (double)gTicksPerSec;
}

int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    const UINT8 *x = (const UINT8 *)a, *y = (const UINT8 *)b;
    for (int i = 0; i < 16; i++)
        if (x[i] != y[i]) return 0;
    return 1;
}

static int (*g_tpm_random)(void *buf, size_t len) = NULL;

void efi_register_tpm_random(int (*fn)(void *buf, size_t len)) {
    g_tpm_random = fn;
}

static int efi_rng_protocol_random(void *buf, size_t len) {
    static EFI_GUID rng_guid = EFI_RNG_PROTOCOL_GUID;
    EFI_RNG_PROTOCOL *rng = NULL;

    if (EFI_ERROR(gBS->LocateProtocol(&rng_guid, NULL, (VOID **)&rng)) || !rng) return -1;
    return EFI_ERROR(rng->GetRNG(rng, NULL, len, buf)) ? -1 : 0;
}

int efi_random(void *buf, size_t len) {
    UINT8 *p = (UINT8 *)buf;
    int rng_ok = (efi_rng_protocol_random(buf, len) == 0);
    int tpm_ok = 0;

    if (g_tpm_random) {
        if (rng_ok) {
            UINT8 tpm_buf[64];
            size_t left = len;
            size_t off = 0;
            tpm_ok = 1;
            while (left > 0) {
                size_t chunk = (left > sizeof(tpm_buf)) ? sizeof(tpm_buf) : left;
                if (g_tpm_random(tpm_buf, chunk) != 0) {
                    tpm_ok = 0;
                    break;
                }
                for (size_t i = 0; i < chunk; i++) p[off + i] ^= tpm_buf[i];
                off += chunk;
                left -= chunk;
            }
        } else {
            tpm_ok = (g_tpm_random(buf, len) == 0);
        }
    }

    if (rng_ok || tpm_ok) return 0;
    return -1;
}

/* rwyctl_open.c: keys and unseal shared between rwyctl and rwy-fuse. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>
#include "rwyctl_open.h"
#include "rwy_crypto.h"
#include "tpm_linux.h"

const char *g_passfile, *g_dekfile, *g_tpm;
const char *g_passstring;

int slurp(const char *path, void *buf, size_t max) {
    int fd = open(path, O_RDONLY);
    ssize_t n;
    if (fd < 0) return -1;
    n = read(fd, buf, max);
    close(fd);
    return (int)n;
}

int prompt_pass(const char *prompt_str, char *out, size_t max) {
    struct termios t, o;
    FILE *tty = fopen("/dev/tty", "r+");
    int ok = 0;
    if (!tty) return 0;
    if (tcgetattr(fileno(tty), &o) == 0) {
        t = o; t.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(fileno(tty), TCSAFLUSH, &t);
        fputs(prompt_str ? prompt_str : "Passphrase: ", tty); fflush(tty);
        ok = fgets(out, (int)max, tty) != NULL;
        tcsetattr(fileno(tty), TCSAFLUSH, &o);
        fputc('\n', tty);
    }
    fclose(tty);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok;
}

const char *tpm_dev(void) {
    const char *dev = g_tpm ? g_tpm : getenv("RWY_TPM");
    return dev ? dev : "/dev/tpmrm0";
}

/* Unseal the data key of the TPM keyslot of a member; a failure is only a note, the next key source follows. */
int tpm_dek_dev(rwy_block_dev_t *dev, uint8_t dek[32]) {
    rwy_keyslot_header_t h;
    rwy_tpm_xport_t x;
    const char *tpm = tpm_dev();
    char msg[PATH_MAX + 64];
    int r;
    if (rwy_keyslot_read_from_dev(dev, &h) || h.slots[1].state != RWY_KEYSLOT_STATE_ACTIVE ||
        h.slots[1].slot_type != RWY_KEYSLOT_TYPE_TPM2)
        return -1;
    if (tpm_linux_open(&x, tpm)) {
        if (g_tpm || getenv("RWY_TPM") || errno != ENOENT) {
            snprintf(msg, sizeof(msg), "note: cannot open the TPM %s (%s), trying the next key source", tpm, strerror(errno));
            warn("%s", msg);
        }
        return -1;
    }
    r = rwy_tpmcmd_unseal(&x, 7, h.slots[1].blob, h.slots[1].blob_len, dek);
    tpm_linux_close(&x);
    if (r) {
        if (x.rc) snprintf(msg, sizeof(msg), "note: TPM unseal failed (rc 0x%x), trying the next key source", x.rc);
        else snprintf(msg, sizeof(msg), "note: TPM unseal failed (error %d), trying the next key source", r);
        warn("%s", msg);
    }
    return r;
}

int pass_dek_dev(rwy_block_dev_t *dev, const char *pass, uint8_t dek[32]) {
    rwy_keyslot_header_t h;
    return rwy_keyslot_read_from_dev(dev, &h) || rwy_keyslot_unwrap(&h, pass, dek);
}

/* Check systemd credentials directory for a key or passphrase. */
static int check_credentials(uint8_t dek[32], char *pass, size_t pass_sz, const char **src, int *have) {
    const char *cdir = getenv("CREDENTIALS_DIRECTORY");
    static const char *const names[] = { "runway.dek", "dek", "runway.key", "key", "passphrase" };
    char path[PATH_MAX];
    size_t i;
    if (!cdir) return 0;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        int n;
        snprintf(path, sizeof(path), "%s/%s", cdir, names[i]);
        if (access(path, R_OK)) continue;
        if (strstr(names[i], "dek")) {
            if (slurp(path, dek, 32) == 32) { *have = 1; return 1; }
        } else {
            n = slurp(path, pass, pass_sz - 1);
            if (n > 0) {
                pass[n] = 0;
                pass[strcspn(pass, "\r\n")] = 0;
                *src = pass;
                return 1;
            }
        }
    }
    return 0;
}

/* One source of a key for try number `tries`: the TPM (already in dek), a key file, a passphrase file,
 * RWY_KEY, systemd credentials or the prompt. Sets *have when dek holds the key, else *src the passphrase. Returns 0 or 4 (X_LOCKED). */
int key_source(int tries, int tpm, const char *file, const char *pf, uint8_t dek[32], char *pass,
               const char **src, int *have) {
    *src = 0;
    *have = 0;
    if (tpm) { *have = 1; return 0; }
    if (file) {
        *have = slurp(file, dek, 32) == 32;
        if (*have) return 0;
        warn("cannot read a 32-byte data key from %s", file);
        return 4;
    }
    if (pf) {
        int n = slurp(pf, pass, 255);
        if (n < 0) { warn("cannot read %s", pf); return 4; }
        pass[n] = 0;
        pass[strcspn(pass, "\r\n")] = 0;
        *src = pass;
        return 0;
    }
    if (g_passstring) { *src = g_passstring; return 0; }
    if ((*src = getenv("RWY_KEY"))) return 0;
    if (check_credentials(dek, pass, 256, src, have)) return 0;
    if (tries < 3 && isatty(0) && prompt_pass("Passphrase: ", pass, 256)) { *src = pass; return 0; }
    warn("store is encrypted and no key was given (%s)", "store locked");
    return 4;
}

/* Resolve DEK for a device: explicit passphrase, TPM unseal, key file, env or credentials. */
int rwy_resolve_dev_key(rwy_block_dev_t *dev, const char *key_pass, uint8_t dek[32]) {
    char pass[256];
    const char *src = NULL;
    int have = 0;
    const char *file = g_dekfile ? g_dekfile : getenv("RWY_DEK_FILE");
    const char *pf = g_passfile ? g_passfile : getenv("RWY_KEY_FILE");

    if (key_pass) return pass_dek_dev(dev, key_pass, dek) == 0 ? 0 : -1;
    if (tpm_dek_dev(dev, dek) == 0) return 0;
    if (key_source(3, 0, file, pf, dek, pass, &src, &have) == 0) {
        if (have) return 0;
        if (src) return pass_dek_dev(dev, src, dek) == 0 ? 0 : -1;
    }
    return -1;
}

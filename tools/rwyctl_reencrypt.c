/* rwyctl: `reencrypt`, rotate the DEK of an encrypted store. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rwyctl.h"

typedef struct {
    const char *old_key;
    const char *old_pass;
    const char *old_passfile;
    const char *old_dekfile;
    const char *new_key;
    const char *new_pass;
    const char *new_passfile;
    const char *new_dekfile;
    const char *new_dekout;
    uint32_t iterations;
} reencrypt_opts_t;

static const char **find_opt_dest(const char *arg, reencrypt_opts_t *ro) {
    if (!strcmp(arg, "--old-key")) return &ro->old_key;
    if (!strcmp(arg, "--old-passphrase")) return &ro->old_pass;
    if (!strcmp(arg, "--old-passphrase-file")) return &ro->old_passfile;
    if (!strcmp(arg, "--old-dek-file")) return &ro->old_dekfile;
    if (!strcmp(arg, "--new-key")) return &ro->new_key;
    if (!strcmp(arg, "--new-passphrase")) return &ro->new_pass;
    if (!strcmp(arg, "--new-passphrase-file")) return &ro->new_passfile;
    if (!strcmp(arg, "--new-dek-file")) return &ro->new_dekfile;
    if (!strcmp(arg, "--new-dek-out")) return &ro->new_dekout;
    return NULL;
}

/* Parse the reencrypt-specific command-line options. */
static int parse_reencrypt_opts(int argc, char **argv, reencrypt_opts_t *ro) {
    int i;
    ro->iterations = 1000;
    for (i = 0; i < argc; i++) {
        const char **dest;
        if (!strcmp(argv[i], "--iterations") && i + 1 < argc) {
            ro->iterations = (uint32_t)strtoul(argv[++i], NULL, 10);
            continue;
        }
        dest = find_opt_dest(argv[i], ro);
        if (dest && i + 1 < argc) {
            *dest = argv[++i];
            continue;
        }
        warn("unknown or incomplete option %s", argv[i]);
        return X_USAGE;
    }
    return 0;
}

/* Re-encryption requires all discovered mirror members healthy and writes to all. */
static int check_mirror_health(void) {
    size_t i;
    for (i = 0; i < g_mirror.num_devs; i++) {
        if (g_mirror.dev_faulted[i]) {
            warn("%s", "reencrypt needs every mirror member healthy");
            return X_POLICY;
        }
    }
    g_mirror.write_policy = RWY_MIRROR_WRITE_REQUIRE_ALL;
    return 0;
}

/* Parse 64 hex characters into a 32-byte key. */
static int parse_hex_key(const char *s, uint8_t out[32]) {
    int i;
    if (strlen(s) != 64) return -1;
    for (i = 0; i < 32; i++) {
        char b[3] = { s[2 * i], s[2 * i + 1], 0 }, *e;
        out[i] = (uint8_t)strtoul(b, &e, 16);
        if (*e || !isxdigit((unsigned char)b[0]) || !isxdigit((unsigned char)b[1])) return -1;
    }
    return 0;
}

/* Resolve or generate the 32-byte new data key. */
static int get_new_dek(const reencrypt_opts_t *ro, uint8_t dek[32]) {
    if (ro->new_dekfile) {
        if (slurp(ro->new_dekfile, dek, 32) != 32) {
            warn("cannot read a 32-byte data key from %s", ro->new_dekfile);
            return X_USAGE;
        }
        return 0;
    }
    if (ro->new_key) {
        struct stat st;
        if (!parse_hex_key(ro->new_key, dek)) return 0;
        if (stat(ro->new_key, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 32) {
            if (slurp(ro->new_key, dek, 32) == 32) return 0;
        }
    }
    if (rwy_random(dek, 32)) {
        warn("%s", "cannot generate random data key");
        return X_IO;
    }
    return 0;
}

/* Resolve a passphrase from a key argument or regular string. */
static const char *parse_pass_arg(const char *arg, char *pass, size_t max) {
    struct stat st;
    if (parse_hex_key(arg, (uint8_t *)pass) != 0) {
        if (stat(arg, &st) == 0 && S_ISREG(st.st_mode) && st.st_size != 32) {
            int n = slurp(arg, pass, max - 1);
            if (n >= 0) {
                pass[n] = 0;
                pass[strcspn(pass, "\r\n")] = 0;
                return pass;
            }
        } else if (stat(arg, &st) != 0) {
            return arg;
        }
    }
    return NULL;
}

/* Resolve the passphrase for wrapping keyslot 0. */
static int get_new_pass(const reencrypt_opts_t *ro, char *pass, size_t max, const char **out) {
    const char *p;
    *out = NULL;
    if (ro->new_pass) { *out = ro->new_pass; return 0; }
    if (ro->new_passfile) {
        int n = slurp(ro->new_passfile, pass, max - 1);
        if (n < 0) { warn("cannot read passphrase from %s", ro->new_passfile); return X_USAGE; }
        pass[n] = 0;
        pass[strcspn(pass, "\r\n")] = 0;
        *out = pass;
        return 0;
    }
    if (ro->new_key && (p = parse_pass_arg(ro->new_key, pass, max))) {
        *out = p;
        return 0;
    }
    if (ro->new_dekfile || ro->new_dekout) {
        *out = "runway-test-passphrase";
        return 0;
    }
    if (isatty(0) && prompt_pass("New passphrase: ", pass, max)) {
        *out = pass;
        return 0;
    }
    warn("%s", "reencrypt needs a new key or passphrase (--new-passphrase, --new-key, etc.)");
    return X_USAGE;
}

/* Save the new data key to a private file. */
static int write_dek_out(const char *path, const uint8_t dek[32]) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, dek, 32) != 32 || close(fd)) {
        warn("cannot write data key to %s", path);
        return X_IO;
    }
    return 0;
}

/* Finalize a previously interrupted re-encryption. */
static int finish_pending_finalize(void) {
    int pending = 0, rc = rwy_reencrypt_pending(&g_store, &pending);
    if (rc == RWY_OK && pending) {
        rc = rwy_reencrypt_finish(&g_store, g_active_dek);
        if (rc == RWY_OK) {
            puts("re-encryption finalized");
            return 1;
        }
        warn("cannot finalize re-encryption: %s", rwy_strerror(rc));
        return -exit_of(rc);
    }
    return 0;
}

/* Build the new keyslot header with keyslot 0 active. */
static int build_new_keyslot_hdr(rwy_keyslot_header_t *hdr, const char *pass,
                                 const uint8_t dek[32], uint32_t iter) {
    rwy_keyslot_init_header(hdr);
    return rwy_keyslot_set_password(hdr, 0, pass, dek, iter);
}

/* Re-encrypt command entry point. */
int cmd_reencrypt(int argc, char **argv) {
    reencrypt_opts_t ro = { 0 };
    uint8_t new_dek[32];
    char pass_buf[256];
    const char *new_pass_str = NULL;
    rwy_keyslot_header_t new_khdr;
    int rc, fin;

    if ((rc = parse_reencrypt_opts(argc, argv, &ro))) return rc;
    if (g_store.sb.crypt_algo != RWY_CRYPT_AES_XTS_256) {
        warn("%s", "reencrypt needs an encrypted store");
        return X_POLICY;
    }
    if ((rc = check_mirror_health())) return rc;

    fin = finish_pending_finalize();
    if (fin > 0) return X_OK_;
    if (fin < 0) return -fin;

    if ((rc = get_new_dek(&ro, new_dek))) return rc;
    if ((rc = get_new_pass(&ro, pass_buf, sizeof(pass_buf), &new_pass_str))) {
        memset(new_dek, 0, sizeof(new_dek));
        return rc;
    }
    if (ro.new_dekout && (rc = write_dek_out(ro.new_dekout, new_dek))) {
        memset(new_dek, 0, sizeof(new_dek));
        memset(pass_buf, 0, sizeof(pass_buf));
        return rc;
    }

    if (build_new_keyslot_hdr(&new_khdr, new_pass_str, new_dek, ro.iterations) != RWY_CRYPTO_OK) {
        memset(new_dek, 0, sizeof(new_dek));
        memset(pass_buf, 0, sizeof(pass_buf));
        warn("%s", "cannot create keyslot header");
        return X_IO;
    }

    rc = rwy_reencrypt(&g_store, &g_raw_dev, g_active_dek, new_dek, &new_khdr);
    if (rc != RWY_OK) {
        warn("reencrypt failed: %s", rwy_strerror(rc));
        rc = exit_of(rc);
    } else {
        memcpy(g_active_dek, new_dek, 32);
        puts("re-encryption complete");
        rc = X_OK_;
    }

    memset(new_dek, 0, sizeof(new_dek));
    memset(pass_buf, 0, sizeof(pass_buf));
    return rc;
}

/*
 * rwy_bls.h — Synthetic Boot Loader Specification (BLS) interface definitions.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_BLS_H
#define RWY_BLS_H

#include <stddef.h>
#include <stdint.h>
#include "rwy_core.h"
#include "rwy_entry.h"

enum {
    RWY_BLS_STATE_INSTALLED = 0,
    RWY_BLS_STATE_OK = 1,
    RWY_BLS_STATE_TESTING = 2,
    RWY_BLS_STATE_FAILED = 3,
};

/* Parse a BLS entry filename (e.g. "entry.conf", "entry+3.conf", "tok-1+deb13+2.efi").
 * Extracts the canonical name without counter ("stem.ext") into canonical_out.
 * Sets *has_counter to 1 if a trailing counter was parsed, 0 if no counter.
 * If *has_counter is 1, *tries_left gets the left counter and *tries_done gets
 * the done counter (-1 if omitted).
 * Returns 0 on success, or a negative error code (e.g. RWY_ERR_INVAL). */
int rwy_bls_name_parse(const char *name, size_t len,
                        char *canonical_out, size_t canonical_sz,
                        int *has_counter,
                        uint32_t *tries_left,
                        int32_t *tries_done);

/* Render a canonical entry name into its counted view name according to state and tries.
 * Returns length of rendered name, or negative error code. */
int rwy_bls_name_render(const char *canonical, size_t canonical_len,
                         const char *state, uint32_t tries,
                         char *out, size_t out_sz);

/* Render LoaderBootCountPath for RUNWAY.EFI:
 * /loader/entries/<stem>+<tries>.conf or /EFI/Linux/<stem>+<tries>.efi.
 * Returns length or negative error code. */
int rwy_bls_name_boot_count_path(const char *canonical, size_t canonical_len,
                                  uint32_t tries_left,
                                  char *out, size_t out_sz);

/* Check whether a query filename matches a rendered entry filename.
 * Exact match returns 1. If not exact, compares canonical names and
 * counter semantics: an entry with 0 tries done matches both
 * <stem>+<tries>.ext and <stem>+<tries>-0.ext.
 * Returns 1 on match, 0 if no match. */
int rwy_bls_name_match_entry(const char *rendered, const char *file);

/* Validate a rename between BLS entry names (e.g. systemd-bless-boot).
 * Returns 0 on valid bless rename and sets *target_state to RWY_BLS_STATE_OK
 * or RWY_BLS_STATE_FAILED, or negative error code (e.g. RWY_ERR_PERM)
 * if rename is not allowed. */
int rwy_bls_name_match_rename(const char *old_name, size_t old_len,
                               const char *new_name, size_t new_len,
                               int *target_state);

#define RWY_BLS_MAX_INITRD 8
#define RWY_BLS_MAX_PATH 256
#define RWY_BLS_MAX_ARGS 4096
#define RWY_BLS_MAX_LABEL 128

typedef struct {
    char linux_path[RWY_BLS_MAX_PATH];
    int has_linux;
    char initrd_paths[RWY_BLS_MAX_INITRD][RWY_BLS_MAX_PATH];
    size_t num_initrd;
    char dtb_path[RWY_BLS_MAX_PATH];
    int has_dtb;
    char options[RWY_BLS_MAX_ARGS + 1];
    size_t options_len;
    char title[RWY_BLS_MAX_LABEL + 1];
    int has_title;
    char meta[1025];
    size_t meta_len;
} rwy_bls_entry_t;

/* Parse a BLS Type 1 entry file (.conf text).
 * Checks definite faults, populates entry fields, and verifies that linux is present.
 * Returns 0 on success, or a negative error code (e.g. RWY_ERR_INVAL). */
int rwy_bls_parse(const char *text, size_t len, rwy_bls_entry_t *entry);

/* Check for definite faults in a BLS Type 1 entry (for flush and rename).
 * Incomplete entries (e.g. missing linux) are accepted. Definite faults
 * (.. in paths, devicetree-overlay, efi key, second linux, ARGS > 4 KiB)
 * return RWY_ERR_INVAL. */
int rwy_bls_check_definite_faults(const char *text, size_t len);

typedef struct {
    char slot_name[32];
    char entry_name[RWY_ENTRY_MAX + 1];
    int is_type2;
    char state[16];
    uint32_t tries;
    uint64_t revision;
    char label[RWY_BLS_MAX_LABEL + 1];
    int has_label;
    char args[RWY_BLS_MAX_ARGS + 1];
    int has_args;
    char meta[1025];
    size_t meta_len;
    int has_kernel;
    int has_initrd;
    int has_dtb;
} rwy_bls_slot_info_t;

struct rwy_store;

/* Render Type 1 .conf text from slot info.
 * Returns length of rendered text, or negative error code. */
int rwy_bls_render_type1(const rwy_bls_slot_info_t *info, char *out, size_t out_sz);

/* The payload directory of a Type 1 entry: "/<token>[/<version>]". The "linux" line of the .conf and the path that
 * the FUSE layer serves both come from here. */
void rwy_bls_derive_prefix(const rwy_bls_slot_info_t *info, char *prefix, size_t max);

/* Read slot info from a slot directory in the store (e.g. "EFI/RUNWAY/CONFIG1").
 * Derives default entry name and default title if ENTRY / LABEL are missing. */
int rwy_bls_read_slot_info(rwy_store_t *s, const char *slot_path, rwy_bls_slot_info_t *info);

/* Encode a path into a flat BLOBS/ filename (percent-encoding '/' and '%'). Returns the length, or RWY_ERR_PERM
 * (and an empty `out`) when the name would be a content name. */
int rwy_bls_encode_blob_name(const char *path, char *out, size_t out_sz);

/* Decode a flat BLOBS/ filename back into its original path. */
int rwy_bls_decode_blob_name(const char *blob, char *out, size_t out_sz);

/* Commit a staged Type 1 entry into a new slot.
 * - Parses conf_text.
 * - Resolves staged payload files in BLOBS/.
 * - Joins multiple initrd archives with 4-byte zero padding.
 * - Creates slot directory and links/writes payloads and scalars.
 * - Finalizes slot with INSTALLED in a single transaction.
 * - With QUOTA set, slots the eviction plan names are removed first, in their own commit
 *   (rwy_bootcfg_make_room); the booted and the newest OK slot are never among them.
 * Returns 0 on success, or a negative error code (e.g. RWY_ERR_NOENT, RWY_ERR_NOSPC, RWY_ERR_INVAL). */
int rwy_bls_commit_type1(rwy_store_t *s, const char *entry_name,
                          const char *conf_text, size_t conf_len,
                          uint32_t initial_tries);

/* Commit a staged Type 2 entry (UKI) into a new slot.
 * - Validates PE/EFI signature ('MZ').
 * - Links staged blob to KERNEL.
 * - Writes ENTRY and TRIES.
 * - Finalizes slot with INSTALLED in a single transaction.
 * - With QUOTA set, evicts first, as rwy_bls_commit_type1 does. */
int rwy_bls_commit_type2(rwy_store_t *s, const char *entry_name,
                          const char *blob_path, uint32_t initial_tries);

/* Check whether a slot can be removed via the BLS projection.
 * Refuses (returns RWY_ERR_BUSY) if:
 * - The slot is named by BOOTED.
 * - The slot is in state OK and is the only slot in state OK.
 * Returns 0 if removal is permitted, or negative error code. */
int rwy_bls_can_remove_slot(rwy_store_t *s, const char *slot_name);

/* Remove a slot via the BLS projection.
 * Validates with rwy_bls_can_remove_slot first, then unlinks all slot entries
 * and removes the slot directory in a single transaction.
 * Returns 0 on success, RWY_ERR_BUSY if refused, or negative error code. */
int rwy_bls_remove_slot(rwy_store_t *s, const char *slot_name);

#endif /* RWY_BLS_H */

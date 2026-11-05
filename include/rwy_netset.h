/*
 * rwy_netset.h — Plaintext settings of the network flight report.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_NETSET_H
#define RWY_NETSET_H

#include <stddef.h>
#include <stdint.h>

#include "rwy_block.h"
#include "rwy_format.h"

/* The five settings, in the order they are shown. */
typedef enum {
    RWY_LOG_SERVER = 0,
    RWY_LOG_INTERFACE,
    RWY_LOG_ADDRESS,
    RWY_LOG_TIMEOUT,
    RWY_LOG_BACKLOG,
    RWY_LOG_SETTING_COUNT
} rwy_log_setting_t;

typedef struct {
    uint32_t server;    /* IPv4 address of the syslog server */
    uint16_t port;      /* UDP port, 514 by default */
    uint8_t  mac[6];    /* permanent address of the admin interface */
    uint32_t address;   /* static IPv4 address of the device */
    uint32_t gateway;   /* 0 = none */
    uint8_t  prefix;    /* network prefix length */
    uint8_t  timeout;   /* total budget in seconds */
    uint8_t  backlog;   /* committed events sent as context */
    uint8_t  flags;     /* RWY_LOG_SET_* */
} rwy_log_settings_t;

#define RWY_LOG_OK     0
#define RWY_LOG_EMPTY  1
#define RWY_LOG_BAD    2
#define RWY_LOG_IO     3

typedef struct {
    int      status[2];  /* RWY_LOG_* for each copy */
    uint64_t seq[2];     /* sequence of each valid copy */
    unsigned copy;       /* winning copy, valid when pick returned RWY_OK */
    uint64_t sequence;   /* sequence of the winning copy */
    rwy_log_settings_t settings;
} rwy_log_info_t;

/* An unset settings block uses the defaults of its two optional values and has the feature off. */
void rwy_log_settings_init(rwy_log_settings_t *s);

/* Name of a setting ("LOG-SERVER", ...); NULL for an unknown id. */
const char *rwy_log_setting_name(rwy_log_setting_t id);

/* Id of a setting name, or -1 when it is not one of the five. */
int rwy_log_setting_id(const char *name);

/* Check one value and store it in `s`; 0, or -1 with *why set. */
int rwy_log_set(rwy_log_settings_t *s, rwy_log_setting_t id, const char *value, size_t len, const char **why);

/* Remove one setting; 0, or -1 with *why set. Timeout and backlog return to their defaults. */
int rwy_log_unset(rwy_log_settings_t *s, rwy_log_setting_t id, const char **why);

/* 1 when every present value is valid; 0 with *why set otherwise. */
int rwy_log_settings_valid(const rwy_log_settings_t *s, const char **why);

/* The five lines of the read-only view. Returns the length it would need, as rwy_flight_format_entry. */
size_t rwy_log_format(const rwy_log_settings_t *s, char *buf, size_t bufsz);

const char *rwy_log_reason(int status);

/* Inspect both copies and pick the newest valid one. Returns RWY_OK, RWY_ERR_CORRUPT when neither copy is
 * valid, or RWY_ERR_IO on a read error. info is always filled. */
int rwy_log_pick(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks, rwy_log_info_t *info);

/* Write a new copy with the next sequence and flush it. The active copy is never overwritten. */
int rwy_log_write(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                  const rwy_log_settings_t *settings);

/* Virtual file reader for EFI/RUNWAY/LOG. An empty area shows the defaults. */
int rwy_log_vread(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                  uint64_t pos, void *buf, size_t *len, size_t *total_sz);

#endif /* RWY_NETSET_H */

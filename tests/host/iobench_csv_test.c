/* iobench_csv_test — check the exact header and row shape of the benchmark CSV files. */
/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>
#include "../efi/iobench_csv.h"

#define MAX_FIELDS 24
#define MAX_FIELD  256

static int parse_csv(const char *line, char fields[][MAX_FIELD], int max) {
    int nf = 0, quoted = 0;
    size_t len = 0;
    const char *p;

    for (p = line;; p++) {
        char c = *p;

        if (quoted) {
            if (!c) return -1;
            if (c == '"') {
                if (p[1] == '"') {
                    fields[nf][len++] = '"';
                    p++;
                } else {
                    quoted = 0;
                }
            } else {
                if (len + 1 >= MAX_FIELD) return -1;
                fields[nf][len++] = c;
            }
            continue;
        }
        if (c == '"') {
            quoted = 1;
        } else if (c == ',' || c == '\n' || c == 0) {
            fields[nf][len] = 0;
            nf++;
            len = 0;
            if (nf > max) return -1;
            if (c != ',') break;
        } else {
            if (len + 1 >= MAX_FIELD) return -1;
            fields[nf][len++] = c;
        }
    }
    return nf;
}

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1; \
        } \
    } while (0)

static int check_runs(void) {
    static const char want[] =
        "run,disk,status,arch,build,bench_version,firmware,system_model,devpath,"
        "media_id,disk_bytes,block_size,ioalign,write_cache,store_uuid,store_kind,"
        "disk_set,config,started,finished,reason,tsc_hz,timing_note,aes";
    iobench_run_t r = { 0 };
    char buf[2048], fields[MAX_FIELDS][MAX_FIELD];

    CHECK(strcmp(iobench_runs_header(), want) == 0 ||
          strcmp(iobench_runs_header(), "run,disk,status,arch,build,bench_version,firmware,system_model,devpath,"
                                        "media_id,disk_bytes,block_size,ioalign,write_cache,store_uuid,store_kind,"
                                        "disk_set,config,started,finished,reason,tsc_hz,timing_note,aes\n") == 0);
    r.run = "2026-10-22T10:00:00Z";
    r.disk = "PciRoot(0x0)/Pci(0x1,0x0)";
    r.status = "ok";
    r.arch = "x86_64";
    r.build = "local";
    r.bench_version = "1";
    r.firmware = "EDK II";
    r.system_model = "-";
    r.devpath = "PciRoot(0x0)/Pci(0x1,0x0)/HD(1,GPT,aa)";
    r.media_id = 42;
    r.disk_bytes = 123456789;
    r.block_size = 512;
    r.ioalign = 4;
    r.write_cache = 1;
    r.store_uuid = "01234567-89AB-CDEF-0123-456789ABCDEF";
    r.store_kind = "plain";
    r.disk_set = "boot";
    r.config = "default";
    r.started = "2026-10-22T10:00:00Z";
    r.finished = "2026-10-22T10:00:42Z";
    r.reason = "done";
    r.tsc_hz = 2400000000ULL;
    r.timing_note = "warm, cached";
    r.aes = "hardware";

    CHECK(iobench_format_run(buf, sizeof(buf), &r) == strlen(buf));
    CHECK(buf[strlen(buf) - 1] == '\n');
    CHECK(parse_csv(buf, fields, MAX_FIELDS) == 24);
    CHECK(strcmp(fields[0], r.run) == 0);
    CHECK(strcmp(fields[1], r.disk) == 0);
    CHECK(strcmp(fields[8], r.devpath) == 0);
    CHECK(strcmp(fields[9], "42") == 0);
    CHECK(strcmp(fields[10], "123456789") == 0);
    CHECK(strcmp(fields[11], "512") == 0);
    CHECK(strcmp(fields[12], "4") == 0);
    CHECK(strcmp(fields[13], "1") == 0);
    CHECK(strcmp(fields[14], r.store_uuid) == 0);
    CHECK(strcmp(fields[15], r.store_kind) == 0);
    CHECK(strcmp(fields[16], r.disk_set) == 0);
    CHECK(strcmp(fields[17], r.config) == 0);
    CHECK(strcmp(fields[18], r.started) == 0);
    CHECK(strcmp(fields[19], r.finished) == 0);
    CHECK(strcmp(fields[20], r.reason) == 0);
    CHECK(strcmp(fields[21], "2400000000") == 0);
    CHECK(strcmp(fields[22], r.timing_note) == 0);
    CHECK(strcmp(fields[23], r.aes) == 0);
    return 0;
}

static int check_samples(void) {
    static const char want[] =
        "run,disk,dev,op,size,sample,stat,seconds,bytes,calls_core,calls_dev,"
        "us_per_call,flush_seconds,extents,encrypted,result";
    iobench_sample_t s = { 0 };
    char buf[1024], fields[MAX_FIELDS][MAX_FIELD];

    CHECK(strcmp(iobench_samples_header(), want) == 0 ||
          strcmp(iobench_samples_header(), "run,disk,dev,op,size,sample,stat,seconds,bytes,calls_core,calls_dev,"
                                           "us_per_call,flush_seconds,extents,encrypted,result\n") == 0);
    s.run = "2026-10-22T10:00:00Z";
    s.disk = "PciRoot(0x0)/Pci(0x1,0x0)";
    s.dev = "rwy-blk";
    s.op = "write";
    s.size = 4096;
    s.sample = 0;
    s.stat = "median";
    s.seconds_us = 1234567;
    s.bytes = 16777216;
    s.calls_core = 4096;
    s.calls_dev = 8192;
    s.us_per_call_x100 = 12345;
    s.flush_us = 1000;
    s.extents = 9;
    s.encrypted = 1;
    s.result = "ok";

    CHECK(iobench_format_sample(buf, sizeof(buf), &s) == strlen(buf));
    CHECK(buf[strlen(buf) - 1] == '\n');
    CHECK(parse_csv(buf, fields, MAX_FIELDS) == 16);
    CHECK(strcmp(fields[0], s.run) == 0);
    CHECK(strcmp(fields[1], s.disk) == 0);
    CHECK(strcmp(fields[2], s.dev) == 0);
    CHECK(strcmp(fields[3], s.op) == 0);
    CHECK(strcmp(fields[4], "4096") == 0);
    CHECK(strcmp(fields[5], "0") == 0);
    CHECK(strcmp(fields[6], "median") == 0);
    CHECK(strcmp(fields[7], "1.234567") == 0);
    CHECK(strcmp(fields[8], "16777216") == 0);
    CHECK(strcmp(fields[9], "4096") == 0);
    CHECK(strcmp(fields[10], "8192") == 0);
    CHECK(strcmp(fields[11], "123.45") == 0);
    CHECK(strcmp(fields[12], "0.001000") == 0);
    CHECK(strcmp(fields[13], "9") == 0);
    CHECK(strcmp(fields[14], "1") == 0);
    CHECK(strcmp(fields[15], "ok") == 0);
    return 0;
}

static int check_quotes(void) {
    iobench_run_t r = { 0 };
    char buf[512], fields[MAX_FIELDS][MAX_FIELD];

    r.run = "a,b";
    r.disk = "say \"hi\"";
    r.status = "ok";
    r.arch = "x86_64";
    r.build = "local";
    r.bench_version = "1";
    r.firmware = "fw";
    r.system_model = "-";
    r.devpath = "Pci(0x1,0x0)";
    r.store_uuid = "u";
    r.store_kind = "plain";
    r.disk_set = "boot";
    r.config = "default";
    r.started = "s";
    r.finished = "f";
    r.reason = "";
    CHECK(iobench_format_run(buf, sizeof(buf), &r) == strlen(buf));
    CHECK(parse_csv(buf, fields, MAX_FIELDS) == 24);
    CHECK(strcmp(fields[0], "a,b") == 0);
    CHECK(strcmp(fields[1], "say \"hi\"") == 0);
    CHECK(strcmp(fields[8], "Pci(0x1,0x0)") == 0);
    CHECK(strcmp(fields[20], "") == 0);
    return 0;
}

int main(void) {
    if (check_runs() || check_samples() || check_quotes()) return 1;
    printf("iobench_csv: schema ok\n");
    return 0;
}

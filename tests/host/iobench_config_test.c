/* iobench_config_test — check the benchmark configuration defaults and parser. */
/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>
#include "../efi/iobench_config.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1; \
        } \
    } while (0)

static int check_defaults(void) {
    bench_config_t cfg;
    char text[BENCH_CONFIG_MAX];
    size_t n;

    bench_config_defaults(&cfg);
    CHECK(cfg.layout == 1);
    CHECK(strcmp(cfg.label, "default") == 0);
    CHECK(cfg.rounds == 1);
    CHECK(cfg.warmup == 1);
    CHECK(cfg.pause == BENCH_PAUSE_NEVER);
    CHECK(cfg.payload_mib == 16);
    CHECK(cfg.devices == BENCH_DEV_ALL);
    CHECK(cfg.disks == BENCH_DISKS_INTERNAL);
    CHECK(cfg.include_boot == 0);
    CHECK(cfg.result == BENCH_RESULT_AUTO);
    CHECK(strcmp(bench_config_disk_set_name(&cfg), "internal") == 0);

    n = bench_config_text(text, sizeof(text), &cfg);
    CHECK(n == strlen(text));
    CHECK(strcmp(text, "layout=1 label=default rounds=1 warmup=1 payload_mib=16 devices=all "
                       "disks=internal include_boot=0 result=auto") == 0);
    return 0;
}

static int check_parse(void) {
    static const char input[] =
        "# benchmark settings\r\n"
        " layout = 1 \r\n"
        "label = camp-1\r\n"
        "rounds = 5\r\n"
        "warmup = 0\r\n"
        "payload_mib = 32\r\n"
        "devices = block, rwy-blk , rwy-enc\r\n"
        "disks = boot\r\n"
        "include_boot = 1\r\n"
        "result = store\r\n";
    bench_config_t cfg;
    char text[BENCH_CONFIG_MAX];
    size_t n;

    bench_config_defaults(&cfg);
    CHECK(bench_config_parse(input, &cfg) == 0);
    CHECK(cfg.layout == 1);
    CHECK(strcmp(cfg.label, "camp-1") == 0);
    CHECK(cfg.rounds == 5);
    CHECK(cfg.warmup == 0);
    CHECK(cfg.payload_mib == 32);
    CHECK(cfg.devices == (BENCH_DEV_BLOCK | BENCH_DEV_RWY_BLOCK | BENCH_DEV_RWY_ENC));
    CHECK(cfg.disks == BENCH_DISKS_BOOT);
    CHECK(cfg.include_boot == 1);
    CHECK(cfg.result == BENCH_RESULT_STORE);
    CHECK(strcmp(bench_config_disk_set_name(&cfg), "boot") == 0);

    n = bench_config_text(text, sizeof(text), &cfg);
    CHECK(n == strlen(text));
    CHECK(strcmp(text, "layout=1 label=camp-1 rounds=5 warmup=0 payload_mib=32 "
                       "devices=block,rwy-blk,rwy-enc disks=boot include_boot=1 result=store") == 0);
    return 0;
}

static int check_probe_device(void) {
    bench_config_t cfg;
    char text[BENCH_CONFIG_MAX];

    CHECK(bench_config_parse("devices=all\n", &cfg) == 0);
    CHECK((cfg.devices & BENCH_DEV_PROBE) == 0);
    CHECK(bench_config_parse("devices=block,probe\n", &cfg) == 0);
    CHECK(cfg.devices == (BENCH_DEV_BLOCK | BENCH_DEV_PROBE));
    bench_config_text(text, sizeof(text), &cfg);
    CHECK(strstr(text, "devices=block,probe") != NULL);
    return 0;
}

static int check_all_devices(void) {
    bench_config_t cfg;
    char text[BENCH_CONFIG_MAX];

    bench_config_defaults(&cfg);
    CHECK(bench_config_parse("devices=all\ndisks=internal\ninclude_boot=1\nresult=esp\npause=fail\n", &cfg) == 0);
    CHECK(cfg.devices == BENCH_DEV_ALL);
    CHECK(cfg.disks == BENCH_DISKS_INTERNAL);
    CHECK(cfg.include_boot == 1);
    CHECK(cfg.result == BENCH_RESULT_ESP);
    CHECK(cfg.pause == BENCH_PAUSE_FAIL);
    CHECK(strcmp(bench_config_disk_set_name(&cfg), "internal+boot") == 0);
    bench_config_text(text, sizeof(text), &cfg);
    CHECK(strstr(text, "devices=all") != NULL);
    CHECK(strstr(text, "result=esp") != NULL);
    return 0;
}

static int check_errors(void) {
    static const char *bad[] = {
        "layout=2\n",
        "rounds=0\n",
        "rounds=101\n",
        "warmup=2\n",
        "payload_mib=0\n",
        "label=a b\n",
        "devices=block,nope\n",
        "devices=\n",
        "disks=usb\n",
        "include_boot=yes\n",
        "result=file\n",
        "pause=sometimes\n",
        "unknown=1\n",
        "rounds\n",
        "=1\n",
    };
    bench_config_t cfg;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        bench_config_defaults(&cfg);
        if (bench_config_parse(bad[i], &cfg) == 0) {
            printf("FAIL: accepted %s", bad[i]);
            return 1;
        }
    }
    bench_config_defaults(&cfg);
    CHECK(bench_config_parse("", &cfg) == 0);
    return 0;
}

static int check_text_truncates(void) {
    bench_config_t cfg;
    char text[16];
    size_t n;

    bench_config_defaults(&cfg);
    n = bench_config_text(text, sizeof(text), &cfg);
    CHECK(n > sizeof(text) - 1);
    CHECK(strlen(text) == sizeof(text) - 1);
    return 0;
}

int main(void) {
    if (check_defaults() || check_parse() || check_all_devices() || check_probe_device() || check_errors() ||
        check_text_truncates())
        return 1;
    printf("iobench_config: defaults and parser ok\n");
    return 0;
}

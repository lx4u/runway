# Testing and Benchmarking Guide

This guide describes how to build and run the Runway test suites, how deterministic fault injection and consistency checking work, what each host, FUSE, and QEMU suite covers, and how to run the `iobench` storage and cipher benchmarks.

---

## 1. Running the Test Suites

Configure and build once with Meson:

```sh
meson setup build
meson compile -C build
```

| Command | Suite | What it runs |
|---|---|---|
| `meson test -C build --suite host` | `host` | Fast userspace unit, fault-injection, crypto, CLI, and 20-cycle rotation tests (no root, no QEMU) |
| `meson test -C build --suite fuse` | `fuse` | Linux FUSE mount, synthetic BLS view, content-blob protection, and RPC tests (requires `/dev/fuse`) |
| `meson test -C build --suite qemu` | `qemu` | Full UEFI boot tests on `STORE=block` (XBOOTLDR partition): `native`, `reinstall`, `block`, `blockio`, `partinfo`, `dbglog`, `tpm`, `tpm-multi`, `mirror`, `multivol`, `sdboot`, `loopguard`, `debian`, `luksroot`, and `iobench*` |
| `meson test -C build --suite qemu-file` | `qemu-file` | UEFI boot tests with `STORE=file` (`RUNWAY.DAT` on the FAT ESP): `native-file`, `reinstall-file`, `mirror-file`, and `filebe` |
| `meson test -C build --suite wdt` | `wdt` | Hardware watchdog driver tests (`wdt` and `wdt-boot`) under QEMU/OVMF |
| `meson test -C build --suite soak` | `soak` | Long-running rotation soak (`soak_cycles`, default `10000`) and full power-cut eviction soak (`evict-cut-full`) |
| `meson test -C build demo` | `demo` | Alpine Linux A/B update and watchdog fallback demo under QEMU (requires `meson compile -C build demo-fetch` once) |

Run any individual test by name (`meson test -C build native`, `meson test -C build bootcfgtest`) or list all registered tests with `meson test -C build --list`.

For a pre-merge verification gate with a shorter soak pass:

```sh
meson configure build -Dsoak_cycles=500
meson test -C build --suite host --suite fuse --suite qemu --suite soak
```

### Concurrency and temporary files

- **Host tests** create an isolated per-process working directory `/var/tmp/rwy-test.*` (`tests/host/testdir.c`) and remove it on exit, so host tests run safely in parallel.
- **QEMU tests** share disk images in `build/` and are serialized automatically by Meson (`is_parallel : false`). Never run two `meson test` instances against the same `build/` directory at the same time.

### Cross-architecture testing (`aarch64`)

Configure an `aarch64` build directory with the cross file:

```sh
meson setup build-arm64 --cross-file cross/aarch64-efi.ini
meson compile -C build-arm64
```

- **ARMv8 Crypto Extensions (`aesarmtest`):** `meson test -C build-arm64 aesarmtest` runs the ARMv8 hardware AES-256-XTS implementation under `qemu-aarch64-static`, verifying FIPS 197 vectors and sector encryption/decryption against the portable C implementation and OpenSSL.
- **Cross-architecture on-disk format check (`xarch`):** Build the host tools for `aarch64` and verify byte-for-byte store compatibility between `x86_64` and `aarch64`:

  ```sh
  meson setup build-arm64-tools --cross-file cross/aarch64-efi.ini -Dnative_tools=false
  meson compile -C build-arm64-tools
  A64_BUILD=$PWD/build-arm64-tools/tests/host/bin meson test -C build xarch
  ```

- **ARM64 UEFI QEMU suites:** `meson test -C build-arm64 native reinstall sdboot multivol mirror wdt wdt-boot debian` boots the `aarch64` EFI binaries under `qemu-system-aarch64 -machine virt` with AAVMF firmware. (`tpm` and `tpm-multi` skip on `aarch64` when AAVMF lacks a TCG2 stack.)

---

## 2. Consistency Checking and Fault Injection

### 2.1 `fsck.runway` verification and repair

`tools/fsck/` (`build/tools/fsck.runway`) verifies:
- Both superblock slots, generation selection, area geometry, and per-member UUIDs.
- Metadata block headers and CRC32C checksums across the active inode table, bitmap, indirect blocks, state record, flight recorder, update journal, recovery area, loader area, and network settings.
- Inode fields, extent bounds, absence of overlapping/double-allocated blocks, and exact agreement between the allocation bitmap and physical reachability (`rwy_mark_data_reachable`).
- Directory tree structure, link counts, absence of orphan inodes, and `EFI/RUNWAY/` slot/record/anchor invariants.
- Fragmentation metrics: `frag: N multi-extent objects, live=X blocks, free=Y blocks, largest free run=Z blocks`.

Modes:
- **Read-only check (default / `-n`):** Never modifies the store.
- **Conservative repair (`--repair` / `-a` / `-p` / `-y`):** Reclaims marked-but-unreachable blocks by publishing one GC-equivalent generation, heals a damaged state record or behind `REVISION` counter, and refuses (exit `4`, store bit-identical) on any structural corruption.
- **Offline compaction (`--defrag`):** Rewrites all fragmented live objects into single contiguous runs and publishes one new generation with identical visible contents.

### 2.2 Deterministic fault injection

Every state-changing path in Runway is tested against simulated power cuts at every individual block write and device flush:
- **In-memory fault-injecting block device (`hosttest`, `committest`, `gctest`, `repairtest`, `defragtest`, `bootcfgtest`, `relocatetest`, `reencrypttest`, `recareatest`):** Runs a workload once to count the total `write` and `flush` calls `N`, then re-runs the workload `N` times, failing at call `i` (`1 <= i <= N`) both cleanly and with half-written (torn) blocks. After every cut, the store must mount cleanly, pass `fsck.runway`, and expose either the complete old state or the complete new state—never a partial or corrupt intermediate state.
- **Syscall-level `LD_PRELOAD` fault injection (`tests/host/faultio.c`, `faultio.so`):** Intercepts `pwrite`/`write`/`fsync` in `rwyctl` processes to simulate power cuts during multi-commit quota evictions (`test_evict_cut.sh`), rotation soaks (`test_rotation.sh`), and degraded mirror writes (`test_rwyctl.sh`).
- **QEMU `blkdebug` media errors:** Injects read and write sector errors inside UEFI (`test_native.sh`, `test_mirror.sh`) to verify retry behavior, unrecorded fallback boots, and degraded mirror catch-up refusal.

---

## 3. Host Test Suites (`tests/host/`)

| Test | What it covers |
|---|---|
| `hosttest` | Seed store validation, POSIX attribute persistence across remounts, multi-extent files (>8 extents with indirect blocks), writes split across scattered free runs, deletion reserve on a full store, abort preserving committed blocks, state record ping-pong and torn-write recovery, recovery/flight/journal superblock geometry, and deterministic fault injection across create/write/commit and UKI atomic rename |
| `committest` | Exact device call sequence of a commit (inode table run, bitmap run, flush, superblock slot, flush), torn/failed write recovery at each step, mirror run-write policies, and core I/O counters |
| `gctest` | Reachability-based space reclamation (`rwy_gc`), 200 MiB large-file lifecycle, 256 MiB cap, fragmented reuse, and fault injection across all 75 GC write/flush points |
| `repairtest` | `fsck.runway --repair` reclamation of unreachable blocks, no-op on clean stores, refusal (store bit-identical) across all six structural corruption classes, and fault injection across all 75 repair points |
| `defragtest` | `fsck.runway --defrag` compaction of multi-extent files into single runs, `NOSPC` recovery on scattered free space, 200 MiB file preservation under the 2x CoW constraint, corruption refusal, and fault injection across all 83 defrag points |
| `bootcfgtest` | `EFI/RUNWAY/` schema and actor rules, slot finalization, virtual files (`STATE`, `TRIES`, `CAUSE`, `BOOTED`, `REASON`, `RECOVER_NEXT`, `INDEX`, `FLIGHT`), slot `LABEL` and `PIN`, `SLOTS` recycling, `QUOTA` eviction planning, boot selection and `condemn`, sealed `RECOVERY/` and `WDT/`, content-addressed `BLOBS/` protection and orphan sweep, state record and TPM anti-rollback anchor (`rev_hw` and `epoch`), and fault injection across every path |
| `cryptotest` | SHA-256, HMAC-SHA256, PBKDF2, AES-256-XTS (hardware AES-NI/ARMv8 and portable C fallback) against NIST/OpenSSL vectors, seeded HMAC-DRBG reproducibility, keyslot header validation, and encrypted in-place state record rewrites under torn writes |
| `relocatetest` | Key-agnostic live-block relocation (`rwy_relocate`) of files, directories, symlinks, and xattrs, `NOSPC` pre-flight abort, and fault injection across all 1,391 write/flush points |
| `reencrypttest` | Full store re-encryption (`rwy_reencrypt`) from `DEK_factory` to `DEK_runtime`, raw-block scrubbing verification (zero factory ciphertext leaks), interrupted finalization resumption, and fault injection |
| `mirrortest` | Multi-device mirror layer, per-member UUID assignment, state record synchronization (`rwy_record_sync`), health counter merging (`LAGGING`, `SYNC_FAILED`, `WRITE_FAILED`, `MISSING`), and member roster management |
| `flighttest` | Plaintext flight recorder ring buffer: A/B ping-pong staging, sequence monotonicity, torn-block fallback, circular wrap-around, and uncommitted `rec_seq` filtering |
| `transplogtest` | Encrypted update journal: rolling SHA-256 hash chain verification, payload digest and epoch binding, tamper detection, circular wrap-around, and dual-binding atomicity (`generation` and `rec_seq`) |
| `recareatest`, `bundletest`, `loaderfsmtest` | Flat bundle builder/verifier, plaintext recovery area (`rwy_recarea.c`), A/B loader area state machine (`rwy_loader_fsm.c`), hostile header rejection, and power-cut safety |
| `blobtest`, `blsviewtest`, `loopguardtest`, `netsettest` | Streaming SHA-256 content-blob naming, cached BLS projection snapshot (`rwy_bls_view.c`), boot loop guard window logic, and plaintext network report settings |
| `gpttest`, `hostdevtest`, `fusestoretest` | Primary/backup GPT parsing in memory, `O_DIRECT` and `IMAGE@OFFSET+LENGTH` window block backend (`tools/hostdev.c`), and in-process FUSE store helper logic |
| `test_rwyctl.sh`, `test_tpm.sh` | End-to-end CLI tests for `rwyctl` and `mkstore`: store discovery (fake sysfs/mountinfo/GPT), `status`/`get`/`slots`, `install`/`update`/`confirm`/`fail`/`remove`, `recover-next`, `check [--blobs] [--repair]`, `recovery set`, `wdt set`, `loader set/show/commit`, `flight show`, `log show/verify/set`, `reencrypt`, `mirror clear/forget`, and `swtpm`-backed TPM keyslot unsealing and `anchor` enrollment |
| `test_evict.sh`, `test_evict_cut.sh`, `test_rotation.sh` | Storage `QUOTA` and `SLOTS` eviction, shared `BLOBS/` accounting, auto-defragmentation before install, power-cut fault injection at every write of eviction + defrag + install, and multi-cycle random payload rotation soak |
| `test_grep_gate.sh`, `test_tmpdirs.sh` | Repository hygiene gate (zero legacy naming occurrences) and concurrent host test temporary directory cleanup check |

---

## 4. FUSE and QEMU Integration Suites

### 4.1 FUSE suites (`tests/fuse/`, `--suite fuse`)

| Test | What it covers |
|---|---|
| `fuse-index` | Mounting single and mirrored stores with `rwy-fuse`, reading virtual files (`INDEX`, `STATE`, `TRIES`, `BOOTED`), exclusive `flock` coordination, and degraded mirror mounts |
| `fuse-bls` | Synthetic BLS Type 1 (`.conf`) and Type 2 (UKI) projection, `bootctl`/`kernel-install` workflows, encrypted store mounting via passphrase, key file, systemd credentials, and `swtpm` PCR 7 unseal, plus `mount.runway` mirror mounts |
| `fuse-content`, `fuse-bridge-blobs` | Content-addressed `BLOBS/<sha256>` immutability (`EPERM` on direct user mutation) and automatic deduplication of kernels/initrds written through the `/boot` BLS bridge |
| `fuse-view`, `fuse-loop`, `fuse-buffer` | `rwy_bls_view` snapshot caching and invalidation across commits, loop-device mounts, and large buffered writes |

### 4.2 QEMU suites (`tests/qemu/`, `--suite qemu`, `--suite qemu-file`, `--suite wdt`)

The QEMU suites build GPT disk images using `tools/mkesp.sh`, `tests/qemu/mkesp_mirror.sh`, and `tests/qemu/mkdisk_multivol.sh`:
- By default (`STORE=block`), the store resides in an `XBOOTLDR` partition after a 36 MiB FAT32 ESP holding `RUNWAY.EFI`.
- With `STORE=file` (used by `--suite qemu-file`), the store resides in `\RUNWAY.DAT` on the FAT ESP.
- Suites that inspect debug-level loader messages (`mirror`, `multivol`, `tpm`, `tpm-multi`, `dbglog`) boot `RWY_DEBUG.EFI`; user-facing suites (`native`, `wdt`, `debian`, `luksroot`, `demo`) boot the production `RUNWAY.EFI` binary.

| Test | What it covers |
|---|---|
| `native` (`native-file`) | Full native boot lifecycle under OVMF: `ARGS`/`INITRD`/`DTB` handover, `BOOTED` confirmation, `TRIES` countdown to `FAILED` and fallback, sealed `RECOVERY/` (`all_failed` and `RECOVER_NEXT`), full-store state record updates, `blkdebug` write-error retries and unrecorded fallback, corrupt PE condemnation, 64-slot scale, `BOOT-DELAY`, CR/LF console formatting, and flight recorder output |
| `reinstall` (`reinstall-file`) | Replacing and re-creating payloads in a store across reboots |
| `block`, `blockio`, `partinfo`, `filebe`, `dbglog` | Raw `XBOOTLDR` partition discovery vs `RUNWAY.DAT` precedence, mixed partition+file mirrors, `EFI_BLOCK_IO` alignment and read-window unit tests (`BLOCKIO_TEST.EFI`), GPT fallback when firmware omits `EFI_PARTITION_INFO_PROTOCOL` (`PARTINFO_TEST.EFI`), FAT file-truncation guard (`FILEBE_TEST.EFI`), and `\RUNWAY.LOG` hardware diagnostic dump on unmountable boot (`dbglog`) |
| `tpm`, `tpm-multi` | First-boot `DEK_factory -> DEK_runtime` CoW re-encryption and TPM 2.0 PCR 7 enrollment under `swtpm`, power-cut recovery during relocation and finalization, subsequent TPM unseal boots, Secure Boot PCR 7 tamper refusal, TPM NV anti-rollback counter (`anchor`), plaintext recovery area boot when TPM unsealing fails, and multi-disk encrypted mirrors |
| `mirror` (`mirror-file`), `multivol` | Two-disk RAID1 mirror boot, automatic catch-up of lagging members, state record merge across members, degraded boot when a member fails writes (`blkdebug`), per-member UUID assignment, and multi-volume isolation |
| `sdboot` | Chain-loading an unmodified `systemd-boot` binary that reads its configuration and kernels from the Runway Simple File System handle |
| `loopguard` | Boot loop guard (`LOOP-GUARD on`) demoting an `OK` slot after exceeding `LOOP-LIMIT` rapid reboots without confirmation and falling back to an older `OK` slot |
| `wdt`, `wdt-boot` | Standalone UEFI watchdog drivers (`I6300ESB.EFI`, `ITCO.EFI`, `WDAT.EFI`) resetting a hung VM under QEMU (`-watchdog-action reset`), `EFI/RUNWAY/WDT/` probing order, `WDT_HINT` caching, per-slot timeout overrides, and Secure Boot signature verification |
| `debian` (`debian-file`) | Full Debian guest booted via `RUNWAY.EFI` with `/boot` mounted by `boot.mount` (`rwy-fuse`): verifies `bootctl`, `systemd-bless-boot`, `kernel-install add/remove`, Type 1 and Type 2 UKI updates, `rwyctl` over RPC on a mounted `/boot`, and trial boot fallback |
| `luksroot` | End-to-end encrypted appliance with UEFI Secure Boot, `swtpm`, encrypted Runway store, signed UKIs, and a LUKS2 root volume unlocked automatically in the initramfs via a signed PCR 11 policy (`pcr-initrd` / `pcr-system`) plus PCR 7 |
| `demo` | Bootable Alpine Linux image ([docs/demo.md](demo.md)) exercising install, confirm, rolling update, a hanging slot reset by the `i6300esb` hardware watchdog, and automatic fallback |

---

## 5. Storage and Cipher Benchmarking (`iobench`)

`iobench` measures read, write, commit, mount, lookup, and cipher performance across raw firmware protocols and Runway stores on the same disk and boot session.

### 5.1 Devices and workloads

In UEFI (`IOBENCH.EFI`), each pass tests up to five storage targets and diagnostic pseudo-devices:
- **`block`:** Raw `EFI_BLOCK_IO_PROTOCOL` on an unformatted partition range (baseline for partition stores).
- **`file`:** Raw `EFI_FILE_PROTOCOL` on the FAT32 ESP (baseline for file stores).
- **`rwy-blk`:** Unencrypted Runway store on an `XBOOTLDR` partition (`block` backend).
- **`rwy-file`:** Unencrypted Runway store in `RWYBENCH.DAT` on the ESP (`file` backend).
- **`rwy-enc`:** AES-256-XTS encrypted Runway store on a partition (`block` backend).
- **`cipher` and `wpath`:** In-memory AES-256-XTS throughput (`encrypt`, `decrypt`, `encrypt-cache`, `encrypt-bounce`, CPU clock measurement via `APERF`/`MPERF` and dependent-add chains) and step-by-step write-path breakdown (`plain`, `copy`, `encrypt`, `encrypt-copy`).
- **`probe`:** Raw-area latency diagnostics detecting slow firmware calls (>1 ms and >10 ms) across varying idle intervals.

Each Runway target runs:
1. **Throughput cells:** 16 MiB sequential writes (plus commit) and reads in chunk sizes of `4 KiB`, `64 KiB`, `1 MiB`, and `16 MiB`, plus `write-fw` / `read-fw` rows isolating the time spent inside firmware I/O calls.
2. **Lifecycle workloads:** `mount` (superblock read + mount), `lookup` (path resolution), `read-frag` (reading a 16 MiB file split across 16 extents), `state` (4 KiB state write + commit), `install` (16 MiB write in 1 MiB chunks + commit), and `meta` (`rwy_stat`).

### 5.2 Running benchmarks and collecting reports

Run the automated QEMU benchmark suites:

```sh
meson test -C build iobench iobench-image iobench-disks iobench-probe iobench-provision
```

To benchmark real hardware:
1. Build a standalone benchmark image with `sh tests/qemu/mkiobench.sh iobench.img build/efi/IOBENCH.EFI` (or build a multi-disk provisioning image with the `iobench-provision` target, which also runs Linux `O_DIRECT`/`O_DSYNC` baselines via `tools/iobench_linux.c` and records hardware topology in `PERF/HW.TXT`).
2. Write the image to a scratch USB stick or disk, boot it in UEFI mode, and let it complete. Results are saved to `PERF/RUNS.CSV`, `PERF/SAMPLES.CSV`, `PERF/HW.TXT`, and `PERF/CONSOLE.LOG` both in the results store partition and on the ESP (`PERF/`).
3. Collect and format the CSV results on a host:

   ```sh
   sh tests/qemu/iobench_collect.sh out-run1 /dev/sdX
   sh tests/qemu/iobench_csv_table.sh out-run1
   ```

4. To benchmark `rwyctl` and `rwy-fuse` on a Linux host using the same CSV schema and compare against a baseline directory:

   ```sh
   sh tests/qemu/iobench_host.sh out-host
   sh tests/qemu/iobench_csv_table.sh --base out-run1 out-host
   ```

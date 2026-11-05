# Runway

A resilient boot manager and transactional update system for unattended Linux
appliances and edge devices.

Runway ensures devices boot reliably and update safely without human intervention.
It combines a minimal UEFI boot loader stub (`RUNWAY.EFI`), A/B boot loader updates
with trial attempts in the plaintext loader area, A/B OS slot selection with automatic
trial count and fallback, hardware watchdog supervision, and a tamper-evident
encrypted boot store sealed to TPM 2.0.

In Linux, Runway integrates natively with systemd and standard Boot Loader
Specification (BLS Type 1 and Type 2 UKI) tools through a synthetic FUSE
filesystem (`mount -t runway`) and the `rwyctl` command-line utility.

Underneath, Runway stores the boot environment in a crash-safe copy-on-write
filesystem. The store lives in its own `XBOOTLDR` partition, with no outer
filesystem, so the firmware FAT driver is out of the boot decision. Where a
dedicated partition is not possible, the same store can reside in a single file
(`RUNWAY.DAT`) on the EFI System Partition (ESP). Either way, power loss cannot
tear a write or corrupt the boot state.

```text
               +--------------------------------------+
               |         Runway Boot & State          |
               |  A/B slots, BLS projection, watchdog |
               |  TPM anti-rollback, flight recorder  |
               +--------------------------------------+
               |             Runway core              |
               |    CoW transactions, extents,        |
               |    AES-256-XTS & TPM 2.0 sealing     |
               +--------------------------------------+
                      |                      |
                 EFI loader            Linux adapter
                (RUNWAY.EFI)           (FUSE / rwyctl)
                      |                      |
                 Firmware / OS           Linux OS /
                  chain-load          kernel-install
```

## Why a dedicated filesystem?

It can be surprising for a boot loader to have its own filesystem. Standard UEFI boot loaders keep Unified Kernel Images (UKIs) and boot-counting state on the FAT EFI System Partition (ESP), or read kernels from a general-purpose OS filesystem. Neither option holds up well on an unattended device that must survive power loss at any point during an update or trial boot:

- **FAT has no crash safety and relies on fragile firmware write paths.** FAT has no transactions, copy-on-write, or metadata checksums. An unexpected reset while staging a UKI, pruning an old slot, or renaming a boot-assessment file (`+<tries>-<done>.efi`) can corrupt the ESP or leave a torn image. Worse, unattended A/B boot assessment *must* write in UEFI firmware on every trial boot to decrement attempt counters and condemn failed slots, and many firmware `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` (FAT) drivers and `SetVariable` NVRAM implementations are unreliable on the write path.
- **General-purpose filesystems are too complex for pre-boot read-write use.** Filesystems such as ext4, XFS, Btrfs, and ZFS are designed for millions of files, deep B-trees, and multi-terabyte volumes. Running a read-write journaled or B-tree filesystem inside UEFI firmware pulls tens of thousands of lines of log-replay and allocator code into the pre-boot trusted computing base, and dynamic metadata trees can fail with `ENOSPC` when deleting files or updating boot state on a full volume.
- **A UKI and boot store has a tiny, bounded shape.** A boot store only ever holds a handful of large immutable payloads (UKIs, kernels, initrds) plus small configuration files and boot state—bounded to 64–4095 MiB and at most 512 inodes. Simplifying and optimizing specifically for that workload makes the store both crash-safe and small enough to run identically in UEFI firmware and Linux userspace:
  - **Flat, zero-allocation metadata commits:** The entire 512-inode table (35 blocks) and allocation bitmap fit in memory and write as a flat array into two fixed alternating image areas, published by flipping one superblock slot. Firmware needs no journal replay and no B-tree balancing.
  - **In-place boot state independent of free space:** Boot-time state changes (`TRIES` decrements, `FAILED` transitions, `BOOTED` tracking) update a fixed two-block ping-pong state record in place, requiring zero free data blocks and zero filesystem transactions even when the store is 100% full.
  - **Contiguous extents with built-in encryption and mirroring:** Each UKI or kernel sits in a single contiguous extent run of 4 KiB blocks and streams directly through `EFI_BLOCK_IO_PROTOCOL` (with optional AES-256-XTS encryption sealed to TPM 2.0 and multi-disk mirroring) without touching the firmware FAT driver.

## Features

- **A/B boot loader updates & frozen stub.** A minimal, frozen EFI stub (`RUNWAY.EFI`)
  on the ESP loads the full loader (`rwy-loader.efi`) from the store's plaintext
  loader area. New loader versions are staged on trial with attempts and watchdog
  timeout, falling back immediately to the confirmed good copy on boot failure.
- **Resilient A/B boot selection & recovery.** Selects OS slots, counts boot
  attempts, automatically falls back to an older working slot on failure, and
  boots a sealed `RECOVERY/` image (or plaintext recovery area) if all slots fail.
- **Watchdog supervision.** Arms a hardware watchdog in firmware to reset the
  device if the OS kernel hangs or fails to reach userspace.
- **Standard BLS & systemd integration.** The FUSE driver (`mount -t runway`)
  exposes a synthetic Boot Loader Specification (BLS Type 1 `.conf` and Type 2 UKI)
  interface at `/boot`, allowing unmodified OS tools (`bootctl`, `kernel-install`,
  `systemd-bless-boot`) to inspect and update slots transparently.
- **Unified management CLI (`rwyctl`).** Inspect and control slots from the running
  system (including while `/boot` is mounted via FUSE RPC) or offline on a build host:
  list slots, install or update OS images, confirm or fail boot states, and inspect
  the flight recorder and update journal.
- **Network flight report.** Opt-in. On an abnormal boot, `RUNWAY.EFI` sends flight
  recorder events to a UDP syslog server over a dedicated management interface with
  a static IPv4 address and bounded timeout. Normal boots send nothing.
- **Boot loop guard.** Opt-in. With `LOOP-GUARD` enabled, an `OK` slot that reboots
  more than `LOOP-LIMIT` times within `LOOP-WINDOW` without re-confirming health is
  marked `FAILED`, falling back to an older `OK` slot or `RECOVERY`.
- **Storage quota & content-addressed deduplication.** With `QUOTA` or `SLOTS` set,
  installing a new slot automatically evicts the oldest unprotected slots. Optional
  content-addressed deduplication (`--dedup`) shares identical payloads in
  `EFI/RUNWAY/BLOBS/`.
- **Authenticated encryption & anti-rollback.** AES-256-XTS block encryption with
  first-boot CoW re-encryption and TPM 2.0 sealing (PCR 7). A TPM NV monotonic
  counter detects offline store rollbacks and restricts boot to `RECOVERY`.
- **Store mirroring.** Mirror a store across up to 8 disks or partitions. Lagging
  or failing replicas are caught up automatically at boot or isolated on media error
  while the device boots degraded from healthy members.
- **Crash-safe copy-on-write storage & checker.** Atomic superblock commits verified
  by deterministic fault injection at every write and flush point, accompanied by
  `fsck.runway` for offline verification, conservative repair, and compaction.

## Documentation

### For Users (Installing and Using Runway)

- **[User Guide (`docs/usage.md`)](docs/usage.md)** — Installing packages or building from
  source, disk partitioning (`XBOOTLDR` vs `RUNWAY.DAT`), mounting `/boot`, managing slots
  with `rwyctl` and BLS tools, watchdogs, boot loop guard, TPM 2.0 store encryption,
  LUKS2 encrypted root with signed PCR 11 policies, mirroring, and store maintenance.
- **[Demo Image (`docs/demo.md`)](docs/demo.md)** — Building and booting the standalone
  Alpine Linux image to see the full A/B update, hang, hardware watchdog reset, and
  fallback cycle in QEMU or on real x86-64 hardware.
- **Manual pages** — `rwyctl(1)`, `rwy-fuse(1)`, `mount.runway(8)`, and `fsck.runway(8)`
   under `man/`.

### For Developers (Design, Testing, and Contributing)

- **[Contributing Guide (`CONTRIBUTING.md`)](CONTRIBUTING.md)** — Build dependencies,
  repository layout, commit conventions, pre-merge verification gates, and coding style.
- **[Architecture and Design (`docs/design.md`)](docs/design.md)** — On-disk format v11,
  transactional CoW engine, state record, frozen stub and loader area, slot state machine,
  BLS projection snapshot, diagnostic rings, AES-256-XTS & TPM 2.0 sealing, and source map.
- **[Testing and Benchmarks (`docs/testing.md`)](docs/testing.md)** — Running the `host`,
  `fuse`, `qemu`, `qemu-file`, `wdt`, and `soak` suites, cross-architecture (`aarch64`)
  testing, deterministic fault injection, and the `iobench` benchmark suite.

## Quick start

### Build and install

```sh
meson setup build
meson compile -C build
meson test -C build --suite host --suite fuse
```

### Try the interactive QEMU targets or demo

```sh
meson compile -C build run              # boot a test store in QEMU + OVMF
meson compile -C build demo-fetch       # download pinned Alpine packages (once)
meson test -C build demo                # run the 4-boot update & watchdog fallback demo
```

## Scale and limits

- **Store size:** 64 MiB to 4095 MiB (4 KiB blocks), up to 512 inodes and 64 boot slots.
- **Intended scale:** A few large kernel/initrd/UKI payloads (up to 256 MiB per file)
  plus boot configuration and metadata, rather than millions of small files.
- **Copy-on-write headroom:** Replacing a file in a single transaction requires free space
  for the new copy alongside the committed copy until the transaction commits.
- **Deletion reserve:** The last 64 blocks (256 KiB) are reserved for deletions so a full
  store can always remove a file or slot. Slot state transitions (`STATE`, `TRIES`,
  `BOOTED`) update the fixed state record in place and require zero free blocks.
- **Fragmentation:** If scattered free space returns `NOSPC` while free blocks remain,
  run `fsck.runway --defrag` offline to compact multi-extent files into single runs.

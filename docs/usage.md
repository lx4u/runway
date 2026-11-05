# Runway User Guide

This guide explains how to install Runway, provision a boot store, manage OS boot slots and updates, configure hardware watchdogs and boot-loop protection, set up TPM 2.0 encryption and LUKS2 root volumes, and maintain the store.

## 1. Components

| Component | Where it runs | Purpose |
|---|---|---|
| Store partition | GPT partition of type `XBOOTLDR` | Holds the Runway filesystem from byte 0 of the partition, with no outer filesystem |
| `RUNWAY.DAT` | EFI System Partition (FAT) | Fallback store format: the same Runway filesystem inside a single file on the ESP |
| `RUNWAY.EFI` | UEFI firmware | Boot loader (or minimal frozen stub when using the loader area) that opens the store, selects the OS slot, arms the watchdog, and starts the image |
| `rwy-loader.efi` | UEFI firmware (from loader area) | Full Runway loader launched by the frozen `RUNWAY.EFI` stub when A/B loader updates are enabled |
| `mkstore` | Build host or target | Creates and populates a new Runway store |
| `rwyctl` | Running Linux OS or build host | Inspects and manages boot slots, loader bundles, encryption keys, and logs |
| `rwy-fuse` / `mount.runway` | Running Linux OS | Mounts the store at `/boot` and exposes a synthetic Boot Loader Specification (BLS) tree |
| `fsck.runway` | Running Linux OS or build host | Checks, repairs, and defragments a store offline |

See also the manual pages: `rwyctl(1)`, `rwy-fuse(1)`, `mount.runway(8)`, and `fsck.runway(8)`.

---

## 2. Installation and System Setup

### 2.1 Building or installing packages

To build from source (requires `gcc`, `meson`, `ninja`, `gnu-efi`, `libssl-dev`, `libfuse3-dev`, and `python3`):

```sh
meson setup build
meson compile -C build
sudo meson install -C build
```

On Debian-based systems, build and install the binary packages:

```sh
dpkg-buildpackage -us -uc -b
sudo apt install ../runway-tools_*.deb ../runway-fuse_*.deb ../runway-bls_*.deb
```

The three packages split responsibilities cleanly:
- **`runway-tools`**: Installs `rwyctl`, `fsck.runway`, the EFI binaries (`/usr/lib/<arch>/runway/RUNWAY.EFI` and `rwy-loader.efi`), and reference watchdog drivers (`/usr/lib/<arch>/runway/wdt/*.EFI`). When upgraded on a system with a loader area, its post-install script automatically stages `rwy-loader.efi` and the watchdog drivers on trial via `rwyctl loader set`.
- **`runway-fuse`**: Installs `rwy-fuse`, the `/sbin/mount.runway` helper, and the `boot.mount` systemd unit that mounts the active boot store on `/boot`.
- **`runway-bls`**: Installs drop-ins for `systemd-bless-boot.service`, `kernel-install` (`BOOT_ROOT=/boot`, `layout=bls`), and `/etc/profile.d/10-runway.sh` so standard systemd and distribution kernel hooks operate transparently on the Runway store.

### 2.2 Disk layout and creating a store

Runway supports two storage layouts:

1. **Dedicated `XBOOTLDR` partition (recommended):**
   - Partition 1: EFI System Partition (ESP, type `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`, FAT32, ~36–64 MiB), mounted at `/efi`. Holds only `\EFI\BOOT\BOOTX64.EFI` (or `BOOTAA64.EFI`) and `\EFI\RUNWAY\RUNWAY.EFI`.
   - Partition 2: Extended Boot Loader partition (`XBOOTLDR`, type `BC13C2FF-59E6-4262-A352-B275FD6F7172`, 64 MiB to 4095 MiB). Holds the Runway store directly on the raw partition, removing the firmware FAT driver from the boot write path.
2. **Single file on the ESP (fallback):**
   - The store lives at `\RUNWAY.DAT` on the FAT32 ESP alongside `\EFI\BOOT\BOOT<ARCH>.EFI`. Use this when the partition table cannot be changed.

Create a new store with `mkstore`:

```sh
mkstore --size 256 --slots 3 --loader /usr/lib/x86_64-linux-gnu/runway/rwy-loader.efi \
    --wdt /usr/lib/x86_64-linux-gnu/runway/wdt store.img
```

```text
wrote store.img: 268435456 bytes, 12 objects, gen=1, uuid=4ebe7394-9eb5-4e9d-bbca-3f21f79e1a3a
```

Common `mkstore` options:
- `--size MiB`: Store size from `64` to `4095` MiB.
- `--slots N`: Initialize `EFI/RUNWAY/` for native A/B boot selection and cap the slot count (`1` to `64`).
- `--quota[=N]`: Set the maximum percentage of usable store space that slots may occupy (`10` to `95`, default `80` when given without a number).
- `--loader FILE` and `--wdt DIR`: Reserve a plaintext loader area and populate copy A with the full `rwy-loader.efi` binary and hardware watchdog drivers.
- `--encrypt` and `--passphrase SECRET`: Encrypt the store with AES-256-XTS and wrap the factory key in transport keyslot 0 (see Section 5).
- `--recovery KERNEL [--initrd F] [--dtb F] [--args TEXT|@FILE]`: Reserve and populate a plaintext recovery area on an encrypted store.
- `--flight-records N` and `--journal-records M`: Set the capacity of the flight recorder and update journal ring buffers (`0` disables the ring; defaults are `64` and `320`).
- `--add DEST=SRC` and `--efi-boot FILE`: Populate files while building the store (`DEST` under `EFI`, `EFI/BOOT`, `EFI/Linux`, `TEST`, or `PERF`).

Write the store image to the `XBOOTLDR` partition (or copy it to `/efi/RUNWAY.DAT`) and install `RUNWAY.EFI` on the ESP:

```sh
dd if=store.img of=/dev/sda2 bs=4M conv=fsync
install -D -m 0644 /usr/lib/x86_64-linux-gnu/runway/RUNWAY.EFI /efi/EFI/BOOT/BOOTX64.EFI
install -D -m 0644 /usr/lib/x86_64-linux-gnu/runway/RUNWAY.EFI /efi/EFI/RUNWAY/RUNWAY.EFI
```

### 2.3 Specifying a store in tools

All host and target tools (`rwyctl`, `rwy-fuse`, `mount.runway`, `fsck.runway`) accept a store in any of these forms:
- **Automatic discovery (`rwyctl` without `--store` or `--device`, `mount -t runway auto /boot`):** Scans disks using the same rules as `RUNWAY.EFI`. It prefers a partition of type `XBOOTLDR` with a valid superblock; only when a disk has no store partition does it inspect mounted FAT partitions for `RUNWAY.DAT` (next to a store partition, `RUNWAY.DAT` is ignored). Discovery never writes to any partition.
- **Block device or `PARTUUID`:** `--device /dev/sda2` or `--store PARTUUID=<uuid>` (looked up in `/dev/disk/by-partuuid`, or directly in the disk GPT). Block devices are opened with `O_DIRECT` so the kernel page cache never caches stale store blocks. The partition may be larger than the store: only the size in the superblock is used.
- **Store file:** `--store /path/to/store.img` or `/efi/RUNWAY.DAT`.
- **Window inside a larger disk image:** `--store IMAGE@OFFSET+LENGTH` or `IMAGE@OFFSET` (bytes in decimal or `0x` hex, aligned to 4096 bytes). Reads and writes outside the window are refused.

### 2.4 Mounting `/boot` and systemd / BLS integration

The packaged `boot.mount` unit mounts the active store at `/boot` using `mount.runway auto`:

```sh
systemctl enable --now boot.mount
```

- `mount.runway auto` queries `rwyctl members` for the boot store path(s) and invokes `rwy-fuse`.
- Keep an `/etc/fstab` entry for `/efi`. If `/boot` is empty and `/etc/fstab` has neither `/boot` nor `/efi`, `systemd-gpt-auto-generator` generates its own `boot.mount` for the ESP and overrides the packaged unit.
- To point `boot.mount` at specific store paths instead of `auto`, add a systemd drop-in setting `What=/dev/sda2` (or a comma-separated list for a mirror: `What=/dev/sda2,/dev/sdb2`).
- Once mounted at `/boot`, `rwy-fuse` projects the store's native slots as a Boot Loader Specification hierarchy (`/boot/loader/entries/*.conf`, `/boot/EFI/Linux/*.efi`, and per-slot kernel/initrd directories). Unmodified `bootctl`, `kernel-install`, and `systemd-bless-boot` read and update slots through `/boot`.
- For manual or development mounts of a store file:

  ```sh
  rwy-fuse -s -o allow_other,default_permissions store.img /mnt
  ```

### 2.5 Running `rwyctl` on a mounted store

When `rwy-fuse` mounts the store on `/boot`, it holds an exclusive lock on the underlying store members and listens on a restricted Unix domain socket (`/run/rwy-fuse/<uuid>.sock`, or `$RWY_RUN_DIR/<uuid>.sock`).

When you run `rwyctl` against that store, it does not fail with "store busy":
- `rwyctl` connects to the `rwy-fuse` RPC socket and passes its command-line arguments along with open file descriptors (`--kernel`, `--initrd`, `--dtb`, `@FILE`, `stdout`, `stderr`) over `SCM_RIGHTS`.
- `rwy-fuse` executes the command in-process on its mounted store serialized on its event loop, without closing the store, dropping locks, or repeating TPM unseal.
- Every normal store command (`status`, `get`, `slots`, `cat`, `check`, `install`, `update`, `remove`, `confirm`, `fail`, `recover-next`, `set`, `put`, `anchor`, `recovery`, `wdt`, `loader`, `mirror`, `log`, `flight`) works transparently while `/boot` is mounted.
- Offline-only maintenance operations (`init`, `check --repair`, `reencrypt`) require an unmounted store and exit with code `5` ("unmount it first").

### 2.6 Plain store mode (without native slots)

A store created without `--slots` (and before `rwyctl init` is run) has no `EFI/RUNWAY/` directory. In this mode, `RUNWAY.EFI` mounts the crash-safe store, exposes it via the UEFI Simple File System protocol, and chain-loads the default EFI application (`\EFI\BOOT\BOOT<ARCH>.EFI` or the binary set with `mkstore --efi-boot`), allowing existing boot loaders such as `systemd-boot` to run unmodified out of the transactional (and optionally encrypted) store.

---

## 3. Managing Boot Slots (`rwyctl`)

Native boot mode is active whenever `EFI/RUNWAY/` exists and holds at least one finalized slot. If the store was created without `--slots`, initialize native mode with:

```sh
rwyctl init
```

### 3.1 Slots and states

Each slot is a directory `EFI/RUNWAY/CONFIG<n>/` holding `KERNEL` and optionally `INITRD`, `DTB`, `ARGS` (kernel command line), `LABEL` (human-readable description up to 128 bytes), `ENTRY` (BLS entry basename), and `META`. Higher `REVISION` numbers are newer.

| State | Meaning |
|---|---|
| `INSTALLED` | Newly installed, not yet booted |
| `TESTING` | Booted at least once, not yet confirmed by the OS |
| `OK` | Confirmed healthy by the OS |
| `FAILED` | Exhausted all `TRIES`, failed to load, marked failed by the operator, or demoted by the boot loop guard. Final. |

`TRIES` is the number of boot attempts an unconfirmed slot is allowed (default `1`). Each boot of an `INSTALLED` or `TESTING` slot decrements `TRIES` by 1; if `TRIES` reaches 0 without confirmation, the next boot marks the slot `FAILED` and falls back to the newest `OK` slot (or `RECOVERY` if none remain).

### 3.2 Inspecting the store

On a booted system, `rwyctl` automatically locates the boot store. Pass `--store FILE` or `--device DEV` when operating on an image file or non-boot store.

List discovered stores:

```text
$ rwyctl --store store.img stores
UUID                                 MEMBERS GENERATION ENCRYPTED BOOT
c0b3c63a-ea98-40a8-86d4-9040761ed740       1          1        no -
  store.img 1
```

Print member paths (used by `mount.runway`):

```sh
rwyctl members
```

Inspect slots and global store state:

```text
$ rwyctl --store store.img status
SLOT                       REVISION             STATE      TRIES  WATCHDOG   CAUSE           LABEL           ENTRY                    META       PIN
CONFIG1                    1                    OK         2      -          -               -               runway-1.conf            -          yes
CONFIG3                    3                    INSTALLED  1      -          -               -               runway-3.conf            -          -
BOOTED=-
REASON=-
WATCHDOG=30
SLOTS=4
BOOT-DELAY=-
QUOTA=80
LOOP-GUARD=-
LOOP-LIMIT=-
LOOP-WINDOW=-
USED=41% (6612/16110 blocks)
LAST-EVICT=CONFIG2 quota
RECOVER_NEXT=no
COUNTER=3
RECOVERY=valid
RECORD=ok
ANCHOR=off
INCOMPLETE=0
```

Key status fields:
- `BOOTED`: Slot started on the current boot (or `-` if offline).
- `REASON`: Why the target was booted (`normal`, `requested` for `RECOVER_NEXT`, `all_failed`, `invalid_store`, or `boot_loop`).
- `CAUSE`: Why a `FAILED` slot failed (`tries_used_up`, `load_error`, `operator`, `boot_loop`, or `bad_blob`).
- `USED` / `LAST-EVICT`: Percentage of usable data pool blocks in use and the most recent automatic eviction (`quota`, `slots`, or `incomplete`).

Query a single field for scripting (exits `1` if unset):

```text
$ rwyctl --store store.img get CONFIG1 STATE
OK
$ rwyctl --store store.img get REVISION
3
```

List slot names, optionally filtering by exact `LABEL` (useful for finding snapshot slots):

```text
$ rwyctl --store store.img slots --label snapshot-1
CONFIG1
```

Pass `--io-stats` before any command (`rwyctl --io-stats status`) to print block I/O and syscall counters on standard error.

### 3.3 Installing, confirming, and failing slots

`install` writes the complete slot and marks it `INSTALLED` in a single atomic commit. If anything fails, no partial slot is left behind:

```text
$ rwyctl --store store.img install --kernel vmlinuz --initrd initrd.img --args @cmdline --tries 2
CONFIG1 1
```

Options for `install` and `update`:
- `--kernel SRC` (required), `--initrd SRC`, `--dtb SRC`: Path to a payload file, or `blob:<sha256>` for an existing content-addressed blob in `EFI/RUNWAY/BLOBS/`.
- `--dedup`: Deduplicate payloads across slots using content-addressed blobs in `EFI/RUNWAY/BLOBS/<sha256>` (see below).
- `--args TEXT|@FILE`: Kernel command line string or file.
- `--label TEXT|@FILE`: Human-readable label (up to 128 bytes).
- `--entry NAME|@FILE` and `--meta TEXT|@FILE`: Optional BLS entry name and metadata.
- `--tries N`: Trial boot attempts (default `1`).
- `--watchdog SECONDS`: Per-slot hardware watchdog timeout override.
- `--name CONFIG<n>` (`install` only): Explicit slot name; defaults to `CONFIG<highest + 1>`.

#### Content-addressed deduplication (`--dedup` and `BLOBS/`)

By default, `rwyctl install` and `rwyctl update` give each slot its own physical copy of the payload files so candidate and fallback slots do not share data blocks.

When `--dedup` is passed (or when files are installed through the `/boot` BLS FUSE bridge, such as `kernel-install` or copying a UKI), Runway hashes each payload file (64-character lowercase hex SHA-256) and stores it once under `EFI/RUNWAY/BLOBS/<hash>`:
- If the blob already exists, the new slot hardlinks it without writing duplicate blocks, and the shared blocks cost no extra space against `QUOTA`.
- Content-addressed blobs in `BLOBS/` are immutable (`EPERM` on direct modification or deletion through `/boot`) and are automatically garbage-collected in the same transaction when the last slot referencing them is removed or evicted.
- `RECOVERY/` always stores its own dedicated copy of its payload blocks so a damaged shared blob cannot affect recovery.

#### Confirming or failing a slot

Once the new slot boots and the system is healthy, confirm it (either via `systemd-bless-boot.service` when `runway-bls` is installed, or directly with `rwyctl`):

```sh
rwyctl confirm            # marks the booted slot OK
rwyctl fail CONFIG3       # marks CONFIG3 FAILED permanently
```

`confirm` without a slot name acts on the currently booted slot. It is refused (exit `6`) if the slot is already `FAILED` or if `RUNWAY.EFI` booted a fallback slot without recording its state write.

### 3.4 Rolling updates, `SLOTS` cap, `QUOTA`, and pinned slots

Use `rwyctl update` for unattended rolling updates without naming a slot:

```text
$ rwyctl --store store.img update --kernel vmlinuz-new --initrd initrd-new.img --args @cmdline
CONFIG2 2
```

`update` requires either `SLOTS` or `QUOTA` to be configured (`mkstore --slots N`, `rwyctl set SLOTS N`, `mkstore --quota=N`, or `rwyctl set QUOTA N`):
- **Protected slots:** Runway never evicts or recycles the currently booted slot, the newest `OK` slot (plus the second-newest `OK` slot when `RECOVERY/` is absent), any slot currently under trial (`INSTALLED` or `TESTING`), the next slot to boot, or any pinned slot.
- **Under `SLOTS`:** While fewer than `SLOTS` exist, `update` adds a new slot. At `SLOTS`, it recycles one removable slot (`FAILED` first, then the oldest `OK` slot) in a single commit. If more than `SLOTS` exist, `install` and `update` evict older removable slots until the cap is met.
- **Under `QUOTA`:** Before installing the new slot, `install` and `update` plan the removal of old slots (`incomplete`, then `FAILED`, then oldest `OK`, then oldest `INSTALLED`) needed to keep usage within `QUOTA`%. Removals happen in one commit (logged as `EVICT` entries in the update journal), optional defragmentation in a second commit if free space is fragmented, and the new slot in the final commit. If the quota cannot be met without touching protected slots, nothing is removed and `rwyctl` exits with code `7`.

#### Pinned slots

Pin a known-good or golden snapshot slot so automatic eviction, `update`, `remove`, and `rmdir` never delete it:

```sh
rwyctl set CONFIG1 PIN 1   # pin CONFIG1
rwyctl set CONFIG1 PIN 0   # unpin CONFIG1
```

### 3.5 Other configuration commands and boot messages

```sh
rwyctl remove CONFIG2         # delete a slot
rwyctl set WATCHDOG 30        # global hardware watchdog timeout in seconds
rwyctl set SLOTS 4            # maximum number of slots (1 to 64, or 'off')
rwyctl set QUOTA 80           # maximum pool usage percentage (10 to 95, or 'off')
rwyctl set BOOT-DELAY 5       # pause 0 to 30 seconds before starting the kernel
rwyctl set LOOP-GUARD on      # enable boot loop detection for OK slots ('on' or 'off')
rwyctl set LOOP-LIMIT 5       # max entries within LOOP-WINDOW before demoting (2 to 255)
rwyctl set LOOP-WINDOW 600    # loop detection window in seconds (30 to 86400)
rwyctl recover-next           # boot RECOVERY once on the next reboot
rwyctl recover-next --cancel  # cancel a pending one-shot recovery request
```

At boot, `RUNWAY.EFI` prints one `RUNWAY:` line per decision to the console and serial port:

```text
RUNWAY: booting CONFIG42 (rev 42, TESTING, 1 try left) "factory-1.2.3"
RUNWAY: CONFIG41 FAILED, skipped (tries_used_up) "field-test"
RUNWAY: recovery requested, booting RECOVERY "rescue"
RUNWAY: CONFIG42 FAILED, boot loop (more than 5 entries in 600 s), skipped
RUNWAY: no bootable config (all_failed), booting RECOVERY
RUNWAY: CONFIG42 cannot be started (0x<status>), marked FAILED "factory-1.2.3"
RUNWAY: WATCHDOG 30s armed by I6300ESB.EFI
```

---

## 4. Resilience, Watchdog, and Diagnostics

### 4.1 Failures and the `RECOVERY` slot

- If a slot's `KERNEL` fails to load or start (e.g., bad PE image or rejected by Secure Boot), `RUNWAY.EFI` marks the slot `FAILED` (`CAUSE=load_error`) and tries the next slot in the same boot session.
- If `RUNWAY.EFI` cannot write to the state record (e.g., transient media write error), it retries three times and then boots the newest confirmed `OK` slot without recording (never an unconfirmed trial slot).
- When all slots are `FAILED` (or when `rwyctl recover-next` was requested), `RUNWAY.EFI` boots `RECOVERY/`.
- `RECOVERY/` is sealed against normal runtime modifications. Populate or update it offline with `--unlock` and an explicit `--store` or `--device`:

  ```sh
  rwyctl --device /dev/sda2 recovery set --unlock --kernel vmlinuz-rec --initrd initrd-rec.img --args "recovery=1"
  ```

### 4.2 Hardware watchdog supervision

When a global or per-slot `WATCHDOG` timeout is set, `RUNWAY.EFI` probes the standalone UEFI watchdog drivers in `EFI/RUNWAY/WDT/` in sorted order until one detects its hardware and arms the timer just before handing off to the OS kernel. It remembers the working driver in `WDT_HINT` to probe it first on subsequent boots.

Install watchdog drivers into `EFI/RUNWAY/WDT/`:

```sh
rwyctl --device /dev/sda2 wdt set --unlock /usr/lib/x86_64-linux-gnu/runway/wdt/*.EFI
```

Included drivers cover Intel TCO (`ITCO.EFI`), ACPI WDAT (`WDAT.EFI`), Intel 6300ESB (`I6300ESB.EFI`), and TI RTI (`RTI.EFI`). The Alpine demo image ([docs/demo.md](demo.md)) demonstrates a hanging update reset by the hardware watchdog and automatic fallback.

### 4.3 Boot loop guard

An `OK` slot no longer decrements `TRIES`, so a confirmed system that later breaks in userspace (e.g., corrupt root config causing an immediate reboot loop) would otherwise reboot forever.

Enable the boot loop guard with:

```sh
rwyctl set LOOP-GUARD on
```

- `RUNWAY.EFI` tracks consecutive entries into the same `OK` slot inside `LOOP-WINDOW` seconds (default `600`) using the `RunwayBootGuard` EFI variable.
- On entry `LOOP-LIMIT + 1` (default `5 + 1 = 6`) within the window without a health confirmation, `RUNWAY.EFI` marks the slot `FAILED` (`CAUSE=boot_loop`) and immediately boots the next older `OK` slot or `RECOVERY/` (`REASON=boot_loop`).
- If no older `OK` slot and no `RECOVERY/` exist, the slot is not demoted (it remains the only bootable image) and a `LOOP_LIMIT` event is recorded in the flight recorder.
- Running `rwyctl confirm` (or `systemd-bless-boot`) on a healthy boot clears `RunwayBootGuard` without writing to the store.

### 4.4 Loader area and A/B boot loader updates

When a store is created with `mkstore --loader`, it reserves a plaintext loader area containing two state record blocks and two bundle slots (copies A and B) for the full loader (`rwy-loader.efi`) and optional watchdog drivers. The ESP holds only the minimal frozen stub (`RUNWAY.EFI`).

Inspect the loader area:

```text
$ rwyctl --store store.img loader show
member store.img: loader area 2 x 768 blocks at 214
  state record: seq 1, good A, trial none, tries 0, timeout 300s, bad none, reason none, wdt_armed none
  copy A: valid, sequence 1
    LOADER 1126963 bytes sha256 23bfbd55e26d...
  copy B: empty
```

Stage a new loader version on trial (written to the inactive copy across all mirror members):

```text
$ rwyctl loader set /usr/lib/x86_64-linux-gnu/runway/rwy-loader.efi \
    --wdt /usr/lib/x86_64-linux-gnu/runway/wdt --tries 3 --timeout 60
member /dev/sda2: loader set on trial (copy B, 3 tries)
```

On the next boot, the frozen `RUNWAY.EFI` stub decrements the trial counter, arms the watchdog, and starts the candidate loader. Once the candidate loader mounts the store and selects an OS slot, it automatically confirms itself as the new `good` copy (or you can promote it manually with `rwyctl loader commit`). If the candidate fails to start, crashes, or hangs until the watchdog resets the machine, the stub falls back to the confirmed `good` copy.

### 4.5 Flight recorder, update journal, and network flight report

Runway maintains two ring buffers in the store:
1. **Flight recorder (`flight show`):** Plaintext ring buffer recording firmware boot selections (`BOOT`), slot condemnations (`CONDEMN`), try decrements (`TRIES`), recovery requests (`RECOVER_NEXT`), and boot-loop events. Readable without encryption keys.
2. **Update journal (`log show`, `log verify`):** Encrypted, hash-chained audit log of administrative changes (`INSTALL`, `UPDATE`, `CONFIRM`, `FAIL`, `REMOVE`, `EVICT`) bound to the superblock generation, state record sequence, payload SHA-256 digest, and TPM anti-rollback epoch.

```text
$ rwyctl --store store.img flight show
SEQ    EVENT        SLOT         REASON         DETAILS
3      BOOT         CONFIG1      normal         rec_seq=3
2      CONDEMN      CONFIG2      normal         rec_seq=2
1      BOOT         CONFIG2      normal         rec_seq=1

$ rwyctl --store store.img log show
SEQ    EVENT     SLOT      REV   LABEL                CALLER   DETAILS
2      CONFIRM   CONFIG1   1     factory-1.2.3        rwyctl   gen=3
1      INSTALL   CONFIG1   1     factory-1.2.3        rwyctl   payload=3a7b8c9d.. gen=2

$ rwyctl --store store.img log verify
log verify: OK (2 entries)
```

Pass `--raw` to `flight show` or `log show` for machine-parsable `key=value` output.

#### Network flight report (opt-in)

On an abnormal boot (slot condemned, `TESTING` retry, fallback to an older slot or `RECOVERY`, boot loop demotion, or unmountable store), `RUNWAY.EFI` can send the flight recorder events as RFC 5424 UDP syslog messages over a dedicated management interface before starting the OS. Normal boots never touch the network.

```text
$ rwyctl log set LOG-SERVER 192.0.2.10:514
$ rwyctl log set LOG-INTERFACE eth1
$ rwyctl log copy-address eth1
$ rwyctl log test
log test: sent to 192.0.2.10:514 from eth1
```

Settings (stored in plaintext outside the encrypted range so reports still work when the store cannot be unlocked):
- `LOG-SERVER`: IPv4 address and optional UDP port (default `514`).
- `LOG-INTERFACE`: Interface name or MAC address (stored as permanent MAC).
- `LOG-ADDRESS`: Static `ipv4/prefix[,gateway]` (copied from a live interface with `log copy-address`).
- `LOG-TIMEOUT`: Link and transmit budget in seconds (`1` to `30`, default `5`).
- `LOG-BACKLOG`: Number of prior committed flight entries sent as context (`0` to `32`, default `8`).
- Inspect with `rwyctl log settings` or disable with `rwyctl log unset NAME`.

---

## 5. Encryption, TPM 2.0, and Anti-Rollback

### 5.1 Creating and unlocking an encrypted store

Create an AES-256-XTS encrypted store with a factory transport passphrase:

```sh
mkstore --size 256 --encrypt --passphrase secret --slots 2 store.img
```

- **First-boot TPM enrollment:** When booted on a target with UEFI Secure Boot active (`SecureBoot=1`, `SetupMode=0`) and a TPM 2.0, `RUNWAY.EFI` unlocks transport keyslot 0, generates a fresh random Data Encryption Key (`DEK_runtime`), seals it into keyslot 1 bound to TPM PCR 7, re-encrypts all live store blocks in a crash-safe CoW pass, scrubs the factory blocks, and erases transport keyslot 0.
- **Subsequent boots:** `RUNWAY.EFI` unseals the key from the TPM automatically. If Secure Boot is disabled or tampered with, PCR 7 changes and the TPM refuses to unseal the key.
- **Providing keys to `rwyctl` and `rwy-fuse`:** Both tools try key sources in order:
  1. Explicit options: `--dek-file FILE` (32 raw bytes), `--passphrase-file FILE` (`--key-file FILE` / `--key PASS` in `rwy-fuse`).
  2. Environment variables: `RWY_DEK_FILE`, `RWY_KEY`, or systemd credentials (`$CREDENTIALS_DIRECTORY` in `rwy-fuse`).
  3. Automatic TPM 2.0 unseal via `/dev/tpmrm0` (override with `--tpm DEV` or `RWY_TPM`).
  4. Interactive terminal passphrase prompt (`rwyctl` only; exits `4` immediately when not on a TTY).

### 5.2 Plaintext recovery area

An encrypted store can reserve a plaintext recovery area outside the encrypted block range so the device can still boot a recovery environment if TPM unsealing fails (for example after a legitimate firmware or certificate change):

```sh
mkstore --size 256 --encrypt --passphrase secret --recovery vmlinuz-rec --initrd initrd-rec.img --args "recovery=1" store.img
```

Update or inspect the plaintext recovery area on an existing store (requires no encryption key; updates the inactive A/B copy across all mirror members):

```text
$ rwyctl --device /dev/sda2 recovery area set --kernel vmlinuz-rec --initrd initrd-rec.img --args "recovery=1"
member /dev/sda2: recovery area written
$ rwyctl --device /dev/sda2 recovery area show
member /dev/sda2: recovery area 2 x 10 blocks at 144
  copy A: valid, sequence 1
  copy B: empty
  using copy A
    KERNEL 9000 bytes sha256 ...
```

### 5.3 Anti-rollback counter

An encrypted store can bind its state record epoch to a TPM 2.0 NV monotonic counter (under a PCR 7 policy) to detect offline full-disk rollback attacks:

```sh
rwyctl anchor status
rwyctl anchor enroll               # enroll against the live TPM
rwyctl anchor enroll --pcr7 HEX    # prepare offline for a known PCR 7 digest
rwyctl anchor reset                # re-create counter after legitimate recovery
```

When enrolled, every slot installation, removal, or failure increments the epoch and the TPM counter. If an attacker restores an older snapshot of the store, `RUNWAY.EFI` detects the mismatch (`RUNWAY: store rolled back`), refuses to boot normal slots, and boots only `RECOVERY`.

### 5.4 Key rotation (`reencrypt`)

Rotate an unmounted encrypted store's Data Encryption Key and passphrase offline:

```sh
rwyctl --device /dev/sda2 --passphrase-file old.pass reencrypt --new-passphrase-file new.pass
```

`reencrypt` checks that sufficient free space exists for a full copy of live data (exits `7` before writing if not), writes all live blocks under the new key, zeros the old blocks, and updates both superblock slots. If interrupted by power loss, the store remains mountable and `reencrypt` resumes cleanly.

### 5.5 Encrypted root with LUKS2, TPM 2.0, and signed PCR 11 policy

Runway can protect an entire appliance so that only the signed `RUNWAY.EFI` binary sits in plaintext on the ESP. The kernel, initrd, and command line live inside a signed Unified Kernel Image (UKI) in the encrypted Runway store, and the Linux root filesystem is a LUKS2 volume unlocked automatically by `systemd-cryptsetup` in the initrd using the TPM.

#### Cryptographic model

1. **UEFI Secure Boot key (`db`):** Verifies `RUNWAY.EFI` and the UKI before execution. Measured into TPM PCR 7.
2. **PCR 11 signing keys (`pcr-initrd` and `pcr-system`):** `ukify` measures UKI sections into PCR 11 and embeds PCR policy signatures. Instead of sealing LUKS2 to a brittle PCR 11 hash that changes on every kernel update, the LUKS2 root volume is bound to PCR 7 plus the public key of `pcr-initrd` (`--tpm2-public-key`).
   - `pcr-initrd` signs only the `enter-initrd` phase, which unlocks the root filesystem in the initramfs.
   - When the initramfs transitions to the real root, `systemd-pcrphase` extends PCR 11 to `leave-initrd` (signed by `pcr-system`), preventing running userspace from unsealing the LUKS2 root keyslot.
   - Updating the OS only requires installing a newly signed UKI with `rwyctl install`; the LUKS2 header never needs to be re-enrolled.

#### Step-by-step setup

1. **Format or convert the root partition to LUKS2:**

   For a fresh partition:
   ```sh
   cryptsetup luksFormat --type luks2 --pbkdf pbkdf2 /dev/sda3
   ```

   To convert an existing ext4 root partition in place (shrink by at least 16 MiB first to make room for the LUKS2 header):
   ```sh
   resize2fs /dev/sda3 <size-minus-16M>
   cryptsetup reencrypt --encrypt --type luks2 --reduce-device-size 16M /dev/sda3
   ```

2. **Generate the PCR signing key pairs on a build host:**

   ```sh
   openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out pcr-initrd.key
   openssl rsa -pubout -in pcr-initrd.key -out pcr-initrd.pub.pem

   openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out pcr-system.key
   openssl rsa -pubout -in pcr-system.key -out pcr-system.pub.pem
   ```

3. **Enroll the LUKS2 volume with `systemd-cryptenroll`:**

   On the target device:
   ```sh
   systemd-cryptenroll --tpm2-device=auto --tpm2-pcrs=7 \
       --tpm2-public-key=pcr-initrd.pub.pem /dev/sda3
   ```

   Or offline on a build host using the target's exported TPM Storage Root Key (`srk.pub`) and known PCR 7 hash:
   ```sh
   systemd-cryptenroll --unlock-key-file=passphrase.txt \
       --tpm2-device-key=srk.pub --tpm2-pcrs=7:sha256=<pcr7_hash> \
       --tpm2-public-key=pcr-initrd.pub.pem root.img
   ```

4. **Configure `/etc/crypttab` and build the initrd:**

   Add to `/etc/crypttab`:
   ```text
   root UUID=<root-luks-uuid> none tpm2-device=auto
   ```

   Build the initramfs (e.g., with `dracut` including the `systemd`, `systemd-cryptsetup`, and `tpm2-tss` modules) and pass on the kernel command line:
   ```text
   rd.luks.uuid=<root-luks-uuid> rd.luks.options=<root-luks-uuid>=tpm2-device=auto
   ```

5. **Build and sign the UKI with `ukify`:**

   Create `ukify.conf`:
   ```ini
   [UKI]
   Cmdline=root=/dev/mapper/root rw console=tty0 rd.luks.uuid=<root-luks-uuid> rd.luks.options=<root-luks-uuid>=tpm2-device=auto
   SecureBootPrivateKey=secureboot.key
   SecureBootCertificate=secureboot.crt
   SignKernel=yes
   PCRBanks=sha256

   [PCRSignature:initrd]
   PCRPrivateKey=pcr-initrd.key
   PCRPublicKey=pcr-initrd.pub.pem
   Phases=enter-initrd

   [PCRSignature:system]
   PCRPrivateKey=pcr-system.key
   PCRPublicKey=pcr-system.pub.pem
   Phases=enter-initrd:leave-initrd
          enter-initrd:leave-initrd:sysinit
          enter-initrd:leave-initrd:sysinit:ready
   ```

   Build the UKI and install it into the Runway store:
   ```sh
   ukify build --config=ukify.conf --linux=vmlinuz --initrd=initrd.img --output=uki.efi
   rwyctl install --kernel uki.efi --tries 3
   ```

---

## 6. Mirrored and Multi-Volume Stores

### 6.1 Store mirroring

A store can be mirrored across up to 8 disks or partitions that share the same store UUID (and have distinct per-member UUIDs). `RUNWAY.EFI`, `rwyctl`, and `mount.runway auto` discover all members automatically, or you can name them explicitly by repeating `--store` / `--device`:

```text
$ rwyctl --device /dev/sda2 --device /dev/sdb2 stores
UUID                                 MEMBERS GENERATION ENCRYPTED BOOT
c0b3c63a-ea98-40a8-86d4-9040761ed740       2         12        no -
  /dev/sda2 12
  /dev/sdb2 12
```

- Every write updates all healthy members. If a member is missing or behind, `rwyctl` and `rwy-fuse` print a warning, leave the lagging member out, and complete the write on the healthy member(s).
- On the next reboot, `RUNWAY.EFI` automatically catches lagging members up from the newest member. If catch-up fails due to media errors, `RUNWAY.EFI` logs `RUNWAY: mirror: member <n> cannot be caught up` and continues booting from the healthy member.
- Per-member health counters (`LAGGING`, `SYNC_FAILED`, `WRITE_FAILED`, `MISSING`) and member roster state (`online`, `lagging`, `absent`) are tracked in the state record and displayed in `rwyctl status` and `fsck.runway`:

  ```text
  MIRROR-MEMBER=efa748ba-90b3-ff8f-da10-eaf9860f2ce0 STATE=lagging LAGGING=2 SYNC_FAILED=0 WRITE_FAILED=0 MISSING=0 LAST=lagging@12
  ```

- Clear health counters after repairing a disk, or remove a permanently decommissioned disk from the roster:

  ```sh
  rwyctl --device /dev/sda2 --device /dev/sdb2 mirror clear
  rwyctl --device /dev/sda2 mirror forget <member-uuid>
  ```

### 6.2 Multiple stores on one system

If a system holds several independent Runway stores (with different store UUIDs), `RUNWAY.EFI` uses only the store paired with the boot disk/ESP for native boot selection. To manage a non-boot store with `rwyctl`, select it with `--uuid <uuid>` or `--device <dev>`.

---

## 7. Maintenance and Exit Codes

### 7.1 Checking, repairing, and defragmenting

Audit the `EFI/RUNWAY/` slot configuration (works online or offline; exits `8` on violations):

```sh
rwyctl check
rwyctl check --blobs                          # also verify SHA-256 of every content blob in BLOBS/
rwyctl --device /dev/sda2 check --repair --unlock   # offline: repair counter/record and remove orphan blobs
```

Run a full filesystem structure check, conservative repair, or offline compaction with `fsck.runway`:

```sh
fsck.runway /dev/sda2                         # read-only full check
fsck.runway --key PASSPHRASE /dev/sda2        # full check of an encrypted store
fsck.runway --tpm /dev/tpmrm0 /dev/sda2       # also verify TPM anti-rollback counter
fsck.runway --repair /dev/sda2                # offline: reclaim unreachable blocks and heal record/counter
fsck.runway --defrag /dev/sda2                # offline: compact fragmented files into single contiguous runs
```

Run `--repair` and `--defrag` only while the store is unmounted. If heavy fragmentation causes `NOSPC` while free blocks remain, `fsck.runway --defrag` repacks fragmented files into contiguous runs in a single atomic commit. The last 64 blocks (256 KiB) of every store are reserved for deletions so a full store can always remove a slot or file.

### 7.2 Exit codes

#### `rwyctl` exit codes

| Code | Meaning |
|---|---|
| `0` | Success |
| `1` | I/O error, or requested field is not set |
| `2` | Usage or command-line syntax error |
| `3` | No store found, or multiple stores found and none selected |
| `4` | Store locked: no usable encryption key |
| `5` | Store busy (locked by another writer, or command requires an unmounted store) |
| `6` | Operation refused by slot/layout/anchor rules |
| `7` | Insufficient space (`NOSPC` or quota cannot be met) |
| `8` | `check`, `loader show`, `recovery area show`, or `log verify` found integrity violations |

#### `fsck.runway` exit codes

| Code | Meaning |
|---|---|
| `0` | No errors (also returned for a no-op `--repair` or successful `--defrag`) |
| `1` | Errors were corrected by `--repair` |
| `4` | Uncorrected errors remain (corruption found, or repair/defrag refused leaving store untouched) |
| `8` | Operational error (cannot open or lock store, wrong key, or I/O failure) |
| `16` | Usage error |

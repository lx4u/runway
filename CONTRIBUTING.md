# Contributing to Runway

Contributions of every kind are welcome: bug fixes, features, tests,
documentation, and reviews of other people's changes.

Runway is boot-critical code. Devices run unattended, with nobody to
power-cycle them, so a failure must always leave a known good slot that
can boot. Read [docs/design.md](docs/design.md) before you change the
on-disk format, the commit path, or `RUNWAY.EFI`.

## Building from source

Building the EFI binaries, host tools, and FUSE adapter requires `gcc`,
`meson`, `ninja`, `gnu-efi`, `libssl-dev`, `libfuse3-dev`, and `python3`.
Running the QEMU and TPM test suites also requires `qemu`, OVMF,
`mtools`, `dosfstools`, `sfdisk`, and `swtpm`.

```sh
meson setup build
meson compile -C build
```

For `aarch64` EFI binaries, configure a separate build directory with
the cross file:

```sh
meson setup build-arm64 --cross-file cross/aarch64-efi.ini
meson compile -C build-arm64
```

## Repository layout

| Path | Purpose |
| --- | --- |
| `include/` | Public headers: on-disk format (`rwy_format.h`, normative), core API, block-device and crypto interfaces |
| `src/` | Portable filesystem core, AES-256-XTS encryption and TPM 2.0 sealing, boot selection, and UEFI stub/loader ([docs/design.md](docs/design.md#6-source-layout) lists every file) |
| `fuse/` | Linux FUSE adapter (`rwy-fuse`, `mount.runway`) |
| `tools/` | `mkstore`, `rwyctl`, `fsck.runway`, and their host helpers |
| `drivers/` | Standalone UEFI hardware watchdog applications |
| `efi/`, `cross/` | Build rules for EFI binaries and the `aarch64` cross file |
| `tests/` | `host` suites, `fuse` suites, `efi` test apps, and `qemu` harnesses |
| `demo/` | Bootable Alpine Linux image demonstrating the update and fallback cycle |
| `data/`, `debian/`, `man/` | systemd and `kernel-install` integration files, Debian packaging, and manual pages |
| `patches/` | U-Boot patches |
| `docs/` | User guide (`usage.md`), demo guide (`demo.md`), architecture and design (`design.md`), and test guide (`testing.md`) |

## Submitting changes

Work on a branch of your own and open a pull request:

```sh
git checkout -b my-feature-branch
git commit -s
git push origin my-feature-branch
```

Describe what the change is for in the pull request, and say how you
tested it (which of the gates below you ran).

## Commit messages

Commit messages follow the [Conventional Commits][] specification:

```text
<type>[(<scope>)][!]: <summary>

[body]

[footer(s)]
Signed-off-by: Your Name <you@example.com>
```

### Types

One of `build`, `chore`, `ci`, `docs`, `feat`, `fix`, `perf`, `refactor`,
`revert`, `style`, or `test`, lowercase.

### Scopes

The scope is a noun naming the part that changed, and is optional --
omit it rather than invent one for a change that spans the whole
project. The ones in use are:

| Scope | What it covers |
| --- | --- |
| `core` | The filesystem core in `src/`: transactions, allocation, boot configuration, the state record |
| `crypto` | Encryption, keyslots, and the TPM key source |
| `demo` | The Alpine demo image |
| `drivers` | Watchdog driver applications under `drivers/` |
| `efi` | `RUNWAY.EFI`, `rwy-loader.efi`, and the EFI adapter |
| `fsck.runway` | The consistency checker |
| `fuse` | The Linux FUSE adapter |
| `mkstore` | The store maker |
| `qemu` | The QEMU suites under `tests/qemu/` |
| `rwyctl` | The target-side and host control tool |
| `tpm` | The TPM transport and NV counter commands |
| `wdt` | Watchdog arming and its tests |

### Summary

Use the imperative present tense -- "add", not "added" or "adds" -- do
not capitalise the first letter, and do not end with a full stop. Keep
the whole header line under 72 characters.

### Body

The body says *why* the change was made, and what the behaviour was
before it. *How* is what the diff is for. Use simple English and wrap it
at 72 characters. Do not put line counts in it. A commit message does
not refer to plans or documents.

### Breaking changes

Mark them both ways: a `!` after the type or scope, and a
`BREAKING CHANGE:` footer saying what stops working. The disk format may
still change; say so in the message when it does.

### Sign-off

Every commit carries a `Signed-off-by:` line, which `git commit -s`
adds for you. It certifies that you wrote the change, or otherwise have
the right to submit it under the project's licence -- see the
[Developer Certificate of Origin](https://developercertificate.org/).

## Shaping the history

- Documentation and tests go in the same commit as the code they cover.
- Write the test first and see it fail. A bug fix is a `fix` commit with
  a test that was red before it.
- When a change makes a large function hard to follow, split the
  function in a preparatory commit (`refactor`) and make the real change
  on top. Behaviour does not change in a preparatory commit.
- Fixing a commit that is not on `main` yet: make a `fixup!` commit and
  `git rebase --autosquash` soon.
- Fixing a commit that is already on `main`: make a regular `fix` commit
  with a `Fixes:` line, truncating the summary to keep the line within
  72 characters:

  ```text
  Fixes: 1b38e92 ("keep blocks of the committed generation until commit")
  ```

## Gates

Run these before you open a pull request. Say in the pull request which
ones you ran.

| Gate | When |
| --- | --- |
| `meson test -C build --suite host` | Always. Host tests: core, `rwyctl`, `fsck.runway`, crypto, and a short rotation soak |
| `lizard -l c drivers tools src fuse -w` | Always. Your change must add no warning (CCN > 15, length > 1000). Do not raise the limits or add exclusions; if a flat option or command switch must stay over, say so in the commit message |
| `meson test -C build --suite fuse` | When you touch the FUSE adapter, virtual files, or BLS projection |
| `meson test -C build --suite qemu` | When you touch `RUNWAY.EFI`, `rwy-loader.efi`, the EFI adapter, the format, boot selection, or mirroring |
| `meson test -C build --suite qemu-file` | When you touch the ESP file backend (`rwy_filebe.c`) or store discovery |
| `meson test -C build --suite wdt` | When you touch `drivers/wdt/` or watchdog arming |
| `meson test -C build --suite soak` | When you touch allocation, the commit path, GC, or defragmentation (10,000 cycles; pass `-Dsoak_cycles=500` for a shorter pre-merge soak) |
| `meson test -C build demo` | When you touch `demo/` (needs `meson compile -C build demo-fetch` once) |

Meson runs the QEMU suites one at a time because they share image files;
do not start a second `meson test` on the same build directory. Host
tests work in a per-run directory `/var/tmp/rwy-test.*` and remove it on
exit, so two host runs can overlap. Clean up any temporary file you
create under `/var/tmp`, and prefix it with `rwy-`. See
[docs/testing.md](docs/testing.md) for what each suite covers.

## Coding style

Follow the style of the code around what you are changing. Write code
with long-term maintenance in mind, and do not add layers of abstraction
nobody asked for. When there is a choice between a small change and the
right design, the right design wins, even with more churn.

Comments are for what the code cannot say for itself -- why a thing is
done this way, what breaks otherwise. Aim for one line, two or three at
most, in simple English. Do not number steps (`/* 2. do this */`).

Prefer techniques proven in stable filesystems and boot loaders (XFS,
ext4, ZFS, Btrfs, U-Boot, EFI Boot Guard) over inventing your own. Design
for resilience: a failure path must not depend on the thing that failed.

Keep a `.c` file under 1,500 lines. If your change takes a file past
that, split it in a preparatory commit first.

## Licence

Contributions are accepted under the licence of the project.

[Conventional Commits]: https://www.conventionalcommits.org/

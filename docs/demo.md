# Demo image

`meson compile -C build demo` builds `build/tests/qemu/bin/demo/demo.img`, a raw GPT disk that boots Alpine Linux (lts kernel, a small
initramfs) through `RUNWAY.EFI` with two boot slots. It shows the whole update cycle: install, confirm,
update, a slot that hangs, a hardware watchdog that resets it, and the fall back to the last good slot. The same image runs under QEMU and
on real x86-64 UEFI hardware.

## Build

```sh
meson compile -C build demo-fetch   # once: downloads the pinned Alpine packages into build/tests/qemu/bin/demo (about 150 MB)
meson compile -C build demo         # builds build/tests/qemu/bin/demo/demo.img (about 162 MiB)
meson test -C build demo            # builds the image, boots it under QEMU and checks the DEMO: lines (about 1 minute)
```

`demo-fetch` is the only step that uses the network, and no other test run starts it.
The `demo` test prints `SKIP` when `build/tests/qemu/bin/demo/pkg` is missing. The package versions and their sha256 are
pinned in `demo/fetch.sh`; when the mirror drops an old file, update the pin there.

## What happens

Each slot has the same kernel and initramfs; `demo.version=N` on the command line tells `demo/init`
which version it is. Every line below is printed on the serial port and on the screen.

| Boot | Slot | Output | Then |
|---|---|---|---|
| 1 | `CONFIG1` | `DEMO: v1 booted CONFIG1`, `v1 confirmed, installing v2` | `rwyctl confirm`, `rwyctl update` (v2), reboot |
| 2 | `CONFIG2` | `v2 booted CONFIG2`, `v2 confirmed, installing v3` | confirm, update (v3 replaces the oldest slot), reboot |
| 3 | `CONFIG1` | `v3 booted CONFIG1`, `v3 will not confirm, reset the machine` | hangs |
| 4 | `CONFIG2` | `v2 booted CONFIG2`, `v3 failed, v2 is running, demo complete` | idles |

The store holds the three WDT drivers (`I6300ESB.EFI`, `ITCO.EFI`, `WDAT.EFI`) in `EFI/RUNWAY/WDT/`. Only the
v3 slot has a `WATCHDOG` (20 s), so boots 1 and 2 arm nothing. At boot 3 `RUNWAY.EFI` tries the drivers; the one
that finds its hardware arms it, and 20 seconds after the hang the machine resets. `TRIES` is used up, so boot 4
runs v2. On a machine where no driver finds a watchdog, `RUNWAY.EFI` prints `WATCHDOG 20s not armed` and you
reset the machine yourself (power button or reset). `RUNWAY.EFI` waits 3 seconds
(`BOOT-DELAY`) before it starts a slot, so its `RUNWAY:` lines can be read on a screen.

## Run under QEMU

Add `-device i6300esb -watchdog-action reset` to let the watchdog reset the VM at boot 3.

```sh
cp /usr/share/OVMF/OVMF_VARS_4M.fd /var/tmp/rwy-demo-vars.fd
qemu-system-x86_64 -machine q35,accel=kvm:tcg -cpu max -m 1024 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=/var/tmp/rwy-demo-vars.fd \
    -drive format=raw,file=build/tests/qemu/bin/demo/demo.img -serial stdio \
    -device i6300esb -watchdog-action reset
```

## First boot on real hardware

1. Write the image to a USB stick or a disk: `dd if=build/tests/qemu/bin/demo/demo.img of=/dev/sdX bs=4M conv=fsync`
   (check the device name twice; this erases it).
2. In the firmware setup, turn Secure Boot off (the binaries are not signed) and boot the stick in UEFI
   mode.
3. Watch the screen or a serial console at 115200 baud. Expect the `RUNWAY:` lines, a 3 second pause, then
   the `DEMO:` lines of boot 1. The machine reboots by itself after boots 1 and 2.
4. At boot 3 (`v3 will not confirm`) the watchdog resets the machine if a driver found one; if not, cut the
   power or press reset. Boot 4 must print
   `v3 failed, v2 is running, demo complete`.
5. To run it again, write the image again.

The initramfs carries drivers for SATA, NVMe, virtio, USB storage and FAT. A machine that needs another
driver stops at the initramfs with a message from `demo/init`; add the module name to `MODS` in
`demo/mkdemo.sh`.

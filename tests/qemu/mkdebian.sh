#!/bin/sh
# tests/qemu/mkdebian.sh — build a Debian guest disk image for QEMU testing:
# a GPT disk whose ESP boots RUNWAY.EFI, mounts the store via FUSE on /boot,
# and starts the Debian userspace. STORE=block (default) keeps the store on an XBOOTLDR
# partition, STORE=file keeps RUNWAY.DAT on the ESP (tools/imglib.sh).
# SPDX-License-Identifier: Apache-2.0
# ROOT=plain (default) leaves the root ext4 partition unencrypted.
# ROOT=luks makes the root a LUKS2 volume enrolled for the TPM, builds the
# dracut initrd and the signed UKI, and signs RUNWAY.EFI.
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
BUILD=${BUILD:-build}
D=${DEBIAN_DIR:-$BUILD/debian}
OUT=${1:-$D/debian.img}
case "$BUILD" in /*) ;; *) BUILD="$(pwd)/$BUILD" ;; esac
case "$D" in /*) ;; *) D="$(pwd)/$D" ;; esac
case "$OUT" in /*) ;; *) OUT="$(pwd)/$OUT" ;; esac
SRC_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
. "$SRC_DIR/tests/qemu/qemu_lib.sh"
ROOT=${ROOT:-plain}
case "$ROOT" in
    plain|luks) ;;
    *) echo "mkdebian: ROOT must be plain or luks, not '$ROOT'" >&2; exit 1 ;;
esac
DEB_ARCH=$QEMU_DEB_ARCH
CACHE_TAR="/var/tmp/rwy-debian-cache-$DEB_ARCH.tar"
[ "$ROOT" = luks ] && CACHE_TAR="/var/tmp/rwy-debian-cache-$DEB_ARCH-luks.tar"
# The guest console is the first serial port: ttyS0 on x86_64, the PL011 on arm64.
CONSOLE=ttyS0
[ "$ARCH" = x86_64 ] || CONSOLE=ttyAMA0
ROOT_LUKS_UUID=${ROOT_LUKS_UUID:-4f3a6e8b-1c2d-4e5f-a6b7-c8d9e0f1a2b3}
CMDLINE="root=LABEL=debian-root rw console=tty0 console=$CONSOLE,115200 systemd.log_target=console"
[ "$ROOT" = luks ] && CMDLINE="$CMDLINE rd.luks.uuid=$ROOT_LUKS_UUID rd.luks.options=$ROOT_LUKS_UUID=tpm2-device=auto"
# A foreign architecture is cross built; the build dependencies are those of the build machine.
DPKG_ARCH=
[ "$ARCH" = x86_64 ] || DPKG_ARCH="-a$DEB_ARCH"
STORE_MB=256
ESP_MB=300
[ "$STORE" = block ] && ESP_MB=$BLOCK_ESP_MB
[ "$ROOT" = luks ] && ESP_MB=$BLOCK_ESP_MB
STORE_PARTUUID=5a0b6e5c-3a57-4c8f-9d1e-7b2f4c6a8e10
ROOT_MB=1024
START=2048


for tool in sfdisk mkfs.vfat mkfs.ext4 mcopy; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "mkdebian: missing $tool" >&2
        exit 0
    fi
done

if [ "$ROOT" = luks ]; then
    for tool in cryptsetup resize2fs systemd-cryptenroll dracut ukify sbsign openssl swtpm; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "mkdebian: missing $tool for ROOT=luks" >&2
            exit 0
        fi
    done
    SNAKE=${SNAKE:-/usr/share/ovmf/PkKek-1-snakeoil}
    if [ ! -f "$SNAKE.key" ] || [ ! -f "$SNAKE.pem" ]; then
        echo "mkdebian: missing snakeoil keys ($SNAKE)" >&2
        exit 0
    fi
fi

for f in "$BUILD/RUNWAY.EFI" "$BUILD/rwy-loader.efi" "$BUILD/mkstore" "$BUILD/rwyctl" "$BUILD/rwy-fuse"; do
    if [ ! -f "$f" ]; then
        echo "mkdebian: missing $f" >&2
        exit 1
    fi
done

mkdir -p "$D"
ROOT_TAR="$D/rootfs.tar"

if [ -f "$ROOT_TAR" ] && [ "$ROOT" = luks ]; then
    if ! tar -tf "$ROOT_TAR" | grep -q 'systemd-cryptsetup'; then
        echo "mkdebian: $ROOT_TAR lacks systemd-cryptsetup, rebuilding for ROOT=luks..."
        rm -f "$ROOT_TAR"
    fi
fi

if [ ! -f "$ROOT_TAR" ]; then
    if [ -f "$CACHE_TAR" ]; then
        cp -f "$CACHE_TAR" "$ROOT_TAR"
    elif command -v mmdebstrap >/dev/null 2>&1; then
        echo "mkdebian: building Debian rootfs with mmdebstrap..."
        DEB_PKGS="linux-image-$DEB_ARCH,systemd,systemd-sysv,systemd-boot,systemd-boot-tools,udev,fuse3,e2fsprogs,kmod,procps"
        [ "$ROOT" = luks ] && DEB_PKGS="$DEB_PKGS,systemd-cryptsetup,cryptsetup-bin,tpm2-tools"
        python3 -c 'import subprocess, os, sys; os.closerange(3, 1024); subprocess.run(sys.argv[1:], check=True)' \
            mmdebstrap --mode=unshare --variant=minbase --architectures="$DEB_ARCH" \
                --include="$DEB_PKGS" \
                trixie "$ROOT_TAR"
        cp -f "$ROOT_TAR" "$CACHE_TAR" 2>/dev/null || true
    else
        echo "mkdebian: neither $ROOT_TAR nor mmdebstrap available" >&2
        exit 0
    fi
fi

# Extract kernel and initrd
KREL=$(tar -tf "$ROOT_TAR" | grep -E '^(\./)?boot/vmlinuz-' | head -n 1)
IREL=$(tar -tf "$ROOT_TAR" | grep -E '^(\./)?boot/initrd.img-' | head -n 1)
[ -n "$KREL" ] && [ -n "$IREL" ] || { echo "mkdebian: kernel/initrd not found in $ROOT_TAR" >&2; exit 1; }

mkdir -p "$D/boot"
tar -xf "$ROOT_TAR" -C "$D" "$KREL" "$IREL"
KFILE="$D/$KREL"
IFILE="$D/$IREL"

if [ "$ROOT" = luks ]; then
    KVER="${KREL##*vmlinuz-}"
    tar -xf "$ROOT_TAR" -C "$D" "./usr/lib/modules/$KVER" 2>/dev/null || tar -xf "$ROOT_TAR" -C "$D" "usr/lib/modules/$KVER"
    DRACUT_INITRD="$D/boot/initrd.img-$KVER"
    unshare -rm sh -c '
        TMPD=$(mktemp -d /var/tmp/rwy-dracut-ns-XXXXXX)
        trap "rm -rf $TMPD" EXIT
        touch "$TMPD/shadow" "$TMPD/gshadow"
        mount --bind "$TMPD/shadow" /etc/shadow
        mount --bind "$TMPD/gshadow" /etc/gshadow
        mount -t tmpfs tmpfs /var/lib/dracut
        DRACUT_KMODDIR_OVERRIDE=1 dracut --no-hostonly --kver "'"$KVER"'" \
            -k "'"$D/usr/lib/modules/$KVER"'" \
            -a "systemd systemd-cryptsetup tpm2-tss" \
            --force "'"$DRACUT_INITRD"'"
    '
    rm -rf "$D/usr"
    IFILE="$DRACUT_INITRD"

    PCR_INITRD_KEY="$D/pcr-initrd.key"
    PCR_INITRD_PUB="$D/pcr-initrd.pub"
    PCR_SYSTEM_KEY="$D/pcr-system.key"
    PCR_SYSTEM_PUB="$D/pcr-system.pub"
    if [ ! -f "$PCR_INITRD_KEY" ]; then
        openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$PCR_INITRD_KEY" 2>/dev/null
        openssl rsa -pubout -in "$PCR_INITRD_KEY" -out "$PCR_INITRD_PUB" 2>/dev/null
    fi
    if [ ! -f "$PCR_SYSTEM_KEY" ]; then
        openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$PCR_SYSTEM_KEY" 2>/dev/null
        openssl rsa -pubout -in "$PCR_SYSTEM_KEY" -out "$PCR_SYSTEM_PUB" 2>/dev/null
    fi

    SKEY=$(mktemp /var/tmp/rwy-sbkey-XXXXXX)
    openssl pkey -in "$SNAKE.key" -passin pass:snakeoil -out "$SKEY" >/dev/null 2>&1

    UKIFY_CONF="$D/ukify.conf"
    cat << EOF > "$UKIFY_CONF"
[UKI]
Cmdline=$CMDLINE
SecureBootPrivateKey=$SKEY
SecureBootCertificate=$SNAKE.pem
SignKernel=yes
PCRBanks=sha256

[PCRSignature:initrd]
PCRPrivateKey=$PCR_INITRD_KEY
PCRPublicKey=$PCR_INITRD_PUB
Phases=enter-initrd

[PCRSignature:system]
PCRPrivateKey=$PCR_SYSTEM_KEY
PCRPublicKey=$PCR_SYSTEM_PUB
Phases=enter-initrd:leave-initrd
       enter-initrd:leave-initrd:sysinit
       enter-initrd:leave-initrd:sysinit:ready
EOF

    UKI_FILE="$D/uki.efi"
    ukify build --linux="$KFILE" --initrd="$IFILE" --config="$UKIFY_CONF" -o "$UKI_FILE" >/dev/null

    CMDLINE2="$CMDLINE test_case=p5"
    UKIFY2_CONF="$D/ukify2.conf"
    sed "s|^Cmdline=.*|Cmdline=$CMDLINE2|" "$UKIFY_CONF" > "$UKIFY2_CONF"
    UKI2_FILE="$D/uki2.efi"
    ukify build --linux="$KFILE" --initrd="$IFILE" --config="$UKIFY2_CONF" -o "$UKI2_FILE" >/dev/null

    UNTRUSTED_KEY="$D/untrusted-sb.key"
    UNTRUSTED_CRT="$D/untrusted-sb.crt"
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$UNTRUSTED_KEY" -out "$UNTRUSTED_CRT" -days 1 -subj "/CN=Untrusted" >/dev/null 2>&1
    UKI_BAD_SB_FILE="$D/uki-bad-sb.efi"
    sbsign --key "$UNTRUSTED_KEY" --cert "$UNTRUSTED_CRT" --output "$UKI_BAD_SB_FILE" "$BUILD/NATIVE_PAYLOAD.EFI" >/dev/null

    PCR_BAD_KEY="$D/pcr-bad.key"
    PCR_BAD_PUB="$D/pcr-bad.pub"
    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$PCR_BAD_KEY" 2>/dev/null
    openssl rsa -pubout -in "$PCR_BAD_KEY" -out "$PCR_BAD_PUB" 2>/dev/null
    UKIFY_BAD_PCR_CONF="$D/ukify-bad-pcr.conf"
    sed -e "s|tpm2-device=auto|tpm2-device=auto,headless=1|" \
        -e "s|^Cmdline=.*|& test_case=n3 rd.timeout=20 systemd.device-timeout=20s|" \
        -e "s|^PCRPrivateKey=$PCR_INITRD_KEY|PCRPrivateKey=$PCR_BAD_KEY|" \
        -e "s|^PCRPublicKey=$PCR_INITRD_PUB|PCRPublicKey=$PCR_BAD_PUB|" "$UKIFY_CONF" > "$UKIFY_BAD_PCR_CONF"
    UKI_BAD_PCR_FILE="$D/uki-bad-pcr.efi"
    ukify build --linux="$KFILE" --initrd="$IFILE" --config="$UKIFY_BAD_PCR_CONF" -o "$UKI_BAD_PCR_FILE" >/dev/null

    rm -f "$SKEY" "$UNTRUSTED_KEY" "$PCR_BAD_KEY"
fi

LOADER_EFI="$BUILD/rwy-loader.efi"
BOOT_EFI="$BUILD/RUNWAY.EFI"
if [ "$ROOT" = luks ]; then
    BOOT_EFI="$D/RUNWAY.signed.efi"
    LOADER_EFI="$D/rwy-loader.signed.efi"
    SKEY=$(mktemp /var/tmp/rwy-sbkey-XXXXXX)
    openssl pkey -in "$SNAKE.key" -passin pass:snakeoil -out "$SKEY" >/dev/null 2>&1
    sbsign --key "$SKEY" --cert "$SNAKE.pem" --output "$BOOT_EFI" "$BUILD/RUNWAY.EFI" >/dev/null
    sbsign --key "$SKEY" --cert "$SNAKE.pem" --output "$LOADER_EFI" "$BUILD/rwy-loader.efi" >/dev/null
    rm -f "$SKEY"
fi

# Build the store (RUNWAY.DAT on the ESP, or the content of the store partition)
STORE_FILE="$D/store.img"
rm -f "$STORE_FILE"
if [ "$ROOT" = luks ]; then
    TPM_DIR="${TPM_DIR:-$D/tpmstate}"
    rm -rf "$TPM_DIR"
    mkdir -p "$TPM_DIR"
    STORE_PASSPHRASE="${STORE_PASSPHRASE:-runway-test-passphrase}"
    "$BUILD/mkstore" --loader "$LOADER_EFI" --size $STORE_MB --slots 2 --encrypt --dek-out "$D/store.dek" "$STORE_FILE" >/dev/null
    RWY_KEY="$STORE_PASSPHRASE" "$BUILD/rwyctl" --store "$STORE_FILE" install --name CONFIG1 \
        --kernel "$UKI_FILE" \
        --entry "debian-guest.conf" --label "Debian Trixie" --tries 3 >/dev/null

    SWTPM_SOCK="$TPM_DIR/hsock"
    SWTPM_CTRL="$TPM_DIR/hctrl"
    rm -f "$SWTPM_SOCK" "$SWTPM_CTRL"
    swtpm socket --tpmstate dir="$TPM_DIR" \
        --server type=unixio,path="$SWTPM_SOCK" \
        --ctrl type=unixio,path="$SWTPM_CTRL" \
        --tpm2 --flags not-need-init,startup-clear >/dev/null 2>&1 &
    HSPID=$!
    for i in $(seq 1 50); do [ -S "$SWTPM_SOCK" ] && break; sleep 0.1; done
    [ -S "$SWTPM_SOCK" ] || { echo "mkdebian: swtpm socket failed to start" >&2; exit 1; }

    PCR7_HASH="df473f69e2c57f805be51fde5edf7b254a588f3e4bb30b661a87fcb6ad8af24c"
    RWY_KEY="$STORE_PASSPHRASE" "$BUILD/rwyctl" --store "$STORE_FILE" --tpm "$SWTPM_SOCK" \
        anchor enroll --pcr7 "$PCR7_HASH" >/dev/null

    kill $HSPID 2>/dev/null || true
    wait $HSPID 2>/dev/null || true
    rm -f "$SWTPM_SOCK" "$SWTPM_CTRL"
else
    "$BUILD/mkstore" --loader "$LOADER_EFI" --size $STORE_MB --slots 2 "$STORE_FILE" >/dev/null
    "$BUILD/rwyctl" --store "$STORE_FILE" install --name CONFIG1 \
        --kernel "$KFILE" --initrd "$IFILE" \
        --args "$CMDLINE" \
        --entry "debian-guest.conf" --label "Debian Trixie" --tries 3 >/dev/null
fi

# Build ESP image
ESP_IMG="$D/esp.img"
rm -f "$ESP_IMG"
truncate -s ${ESP_MB}M "$ESP_IMG"
mkfs_esp "$ESP_IMG" 0 $((ESP_MB * 1024))
mmd -i "$ESP_IMG" ::EFI ::EFI/BOOT
mcopy -i "$ESP_IMG" "$BOOT_EFI" "::EFI/BOOT/$QEMU_BOOT_FILE"

# Build and unpack Debian packages into overlay
PKG_DIR="$D/pkgs"
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR"
(cd "$SRC_DIR" && dpkg-buildpackage $DPKG_ARCH ${DPKG_ARCH:+-d} -b -us -uc)
mv "$SRC_DIR"/../runway*_"$DEB_ARCH".deb "$SRC_DIR"/../runway*_all.deb "$SRC_DIR"/../runway*.buildinfo "$SRC_DIR"/../runway*.changes "$PKG_DIR/" 2>/dev/null || true
(cd "$SRC_DIR" && dpkg-architecture $DPKG_ARCH -c fakeroot debian/rules clean >/dev/null 2>&1)

ODIR="$D/overlay"
rm -rf "$ODIR"
mkdir -p "$ODIR/usr/local/bin" \
         "$ODIR/etc/kernel" \
         "$ODIR/etc/systemd/system/multi-user.target.wants" \
         "$ODIR/boot" \
         "$ODIR/var/cache/runway-pkgs"

for deb in "$PKG_DIR"/runway-tools_*_"$DEB_ARCH".deb "$PKG_DIR"/runway-fuse_*_"$DEB_ARCH".deb "$PKG_DIR"/runway-bls_*.deb; do
    dpkg-deb -x "$deb" "$ODIR"
    cp "$deb" "$ODIR/var/cache/runway-pkgs/"
done

mkdir -p "$ODIR/usr/share"
if [ -n "${UKI2_FILE:-}" ] && [ -f "$UKI2_FILE" ]; then
    cp "$UKI2_FILE" "$ODIR/usr/share/test-uki2.efi"
fi
# ukify uses the stub of the build machine, so a foreign guest takes the objcopy path.
UKI_OBJCOPY=objcopy
[ "$ARCH" = x86_64 ] || UKI_OBJCOPY="$QEMU_MULTIARCH-objcopy"
if [ "$ARCH" = x86_64 ] && command -v ukify >/dev/null 2>&1; then
    ukify build --linux="$KFILE" \
        --cmdline="$CMDLINE" \
        --os-release=@/etc/os-release -o "$ODIR/usr/share/test-uki.efi" >/dev/null 2>&1 || true
fi
if [ ! -f "$ODIR/usr/share/test-uki.efi" ] && command -v "$UKI_OBJCOPY" >/dev/null 2>&1; then
    "$UKI_OBJCOPY" --add-section .osrel=/etc/os-release --add-section .linux="$KFILE" \
        "$BUILD/RUNWAY.EFI" "$ODIR/usr/share/test-uki.efi" 2>/dev/null || true
fi
if [ ! -f "$ODIR/usr/share/test-uki.efi" ]; then
    cp "$KFILE" "$ODIR/usr/share/test-uki.efi"
fi

ESP_OPTS="defaults,nofail"
[ "$ROOT" = luks ] && ESP_OPTS="ro,defaults,nofail"
cat << FSTAB > "$ODIR/etc/fstab"
LABEL=debian-root / ext4 defaults 0 1
LABEL=ESP /efi vfat $ESP_OPTS 0 2
FSTAB

if [ "$ROOT" = luks ]; then
    echo "luks-$ROOT_LUKS_UUID UUID=$ROOT_LUKS_UUID none tpm2-device=auto" > "$ODIR/etc/crypttab"
    ln -s /dev/null "$ODIR/etc/systemd/system/systemd-boot-random-seed.service"
fi

printf 'STORE=%s\nSTORE_PARTUUID=%s\n' "$STORE" "$STORE_PARTUUID" > "$ODIR/etc/runway-guest.conf"
[ "$ROOT" = luks ] && printf 'ROOT=%s\nROOT_LUKS_UUID=%s\n' "$ROOT" "$ROOT_LUKS_UUID" >> "$ODIR/etc/runway-guest.conf"

echo "$CMDLINE" > "$ODIR/etc/kernel/cmdline"

cat << 'KTOK' > "$ODIR/etc/kernel/entry-token"
debian-test
KTOK

# Guest test runner script
TEST_SCRIPT="${DEBIAN_TEST_SCRIPT:-$D/guest_test.sh}"
if [ -f "$TEST_SCRIPT" ]; then
    cp "$TEST_SCRIPT" "$ODIR/usr/local/bin/runway-guest-test.sh"
    chmod 755 "$ODIR/usr/local/bin/runway-guest-test.sh"
else
    cat << 'SVC_SCRIPT' > "$ODIR/usr/local/bin/runway-guest-test.sh"
#!/bin/sh
set -e
export PATH="$PATH:/sbin:/usr/sbin"
export SYSTEMD_RELAX_XBOOTLDR_CHECKS=1
export SYSTEMD_RELAX_ESP_CHECKS=1

. /etc/runway-guest.conf

echo "RUNWAY-GUEST: TEST START"

if [ "${ROOT:-plain}" = luks ]; then
    echo "RUNWAY-GUEST: checking LUKS root"
    ROOT_SRC=$(findmnt -n -o SOURCE /)
    [ -n "$ROOT_SRC" ] || { echo "FAIL: cannot find root source"; exit 1; }
    echo "RUNWAY-GUEST: root source is $ROOT_SRC"
    status_out=$(cryptsetup status "$ROOT_SRC") || { echo "FAIL: cryptsetup status failed"; exit 1; }
    echo "$status_out" | grep -q -i "luks2" || { echo "FAIL: root is not LUKS2"; exit 1; }
    echo "RUNWAY-GUEST: root is verified LUKS2 volume"

    underlying=$(echo "$status_out" | awk '/device:/ {print $2}')
    [ -n "$underlying" ] || { echo "FAIL: cannot determine underlying device"; exit 1; }
    dump_out=$(cryptsetup luksDump "$underlying") || { echo "FAIL: cryptsetup luksDump failed"; exit 1; }
    echo "$dump_out" | grep -q "systemd-tpm2" || { echo "FAIL: missing systemd-tpm2 token"; exit 1; }
    echo "$dump_out" | grep -q -E "tpm2-pubkey|tpm2-public-key" || { echo "FAIL: missing tpm2-pubkey"; exit 1; }
    echo "RUNWAY-GUEST: LUKS2 token and tpm2-public-key verified"

    CRYPTSETUP_BIN=$(command -v systemd-cryptsetup || echo /usr/lib/systemd/systemd-cryptsetup)
    if "$CRYPTSETUP_BIN" attach test-n5 "$underlying" none "tpm2-device=auto,headless=1,tries=1" </dev/null >/dev/null 2>&1; then
        cryptsetup close test-n5 2>/dev/null || true
        echo "FAIL: systemd-cryptsetup attach unexpectedly succeeded in running system"
        exit 1
    fi
    echo "RUNWAY-GUEST: running system cannot unlock root via TPM (N5 verified)"

    sb_ok=0
    if bootctl status 2>/dev/null | grep -q "Secure Boot: enabled"; then
        sb_ok=1
    elif [ -f /sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c ] && \
         od -An -tx1 /sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c 2>/dev/null | grep -q "01"; then
        sb_ok=1
    fi
    [ "$sb_ok" = 1 ] || { echo "FAIL: Secure Boot not enabled"; exit 1; }
    echo "RUNWAY-GUEST: Secure Boot verified enabled"

    [ -z "$(systemctl --failed --no-legend)" ] || { echo "FAIL: failed units"; systemctl --failed; exit 1; }
    echo "RUNWAY-GUEST: guest systemctl clean"

    if grep -q "test_case=p5" /proc/cmdline; then
        echo "RUNWAY-GUEST: CONFIG2 boot verified"
    elif [ -f /var/lib/runway-boot2-done ] && [ -f /usr/share/test-uki2.efi ] && [ ! -f /var/lib/runway-p5-installed ]; then
        STORE_DEV=/dev/disk/by-partuuid/$STORE_PARTUUID
        rwyctl --store "$STORE_DEV" install --name CONFIG2 \
            --kernel /usr/share/test-uki2.efi \
            --entry "debian-guest2.conf" \
            --label "Debian Trixie Slot 2" \
            --tries 3 || { echo "FAIL: rwyctl install CONFIG2 failed"; exit 1; }
        touch /var/lib/runway-p5-installed
        echo "RUNWAY-GUEST: installed CONFIG2 successfully"
    elif [ -f /var/lib/runway-boot1-done ]; then
        touch /var/lib/runway-boot2-done
    else
        touch /var/lib/runway-boot1-done
    fi

    echo "RUNWAY-GUEST: ALL TESTS PASSED"
    poweroff -f
    exit 0
fi

mount | grep -E -q "type (fuse\.rwy-fuse|runway)" || { echo "FAIL: /boot not mounted with rwy-fuse"; exit 1; }
[ -f /boot/loader/entries.srel ] || { echo "FAIL: missing /boot/loader/entries.srel"; exit 1; }

[ "$(cat /boot/loader/entries.srel)" = "type1" ] || { echo "FAIL: invalid entries.srel"; exit 1; }
echo "RUNWAY-GUEST: mount verified"

# The store partition has no file system and rwy-fuse alone holds it
STORE_DEV=/efi/RUNWAY.DAT
if [ "$STORE" = block ]; then
    STORE_DEV=/dev/disk/by-partuuid/$STORE_PARTUUID
    [ -z "$(blkid -p -o value -s TYPE "$STORE_DEV")" ] || { echo "FAIL: the store partition shows a file system"; exit 1; }
    pid=$(pgrep -x rwy-fuse | head -n 1)
    [ -n "$pid" ] || { echo "FAIL: rwy-fuse not running"; exit 1; }
    dev=$(readlink -f "$STORE_DEV")
    for fd in /proc/$pid/fd/*; do
        [ "$(readlink -f "$fd")" = "$dev" ] || continue
        flags=$(awk '/^flags/ {print $2}' "/proc/$pid/fdinfo/${fd##*/}")
        [ $((0$flags & 040000)) -ne 0 ] || { echo "FAIL: $dev is not open with O_DIRECT (flags $flags)"; exit 1; }
        echo "RUNWAY-GUEST: rwy-fuse holds $dev with O_DIRECT (flags $flags)"
        found=1
    done
    [ -n "${found:-}" ] || { echo "FAIL: rwy-fuse does not hold $dev"; exit 1; }
fi
systemctl show -p Type,What,Result,DevicePolicy,PrivateDevices,ProtectSystem boot.mount
[ -z "$(systemctl --failed --no-legend)" ] || { echo "FAIL: failed units"; systemctl --failed; exit 1; }
if journalctl -b --no-pager | grep -i -e "$STORE_PARTUUID" -e sda2 | grep -i -e fail -e error; then
    echo "FAIL: the guest logged an error about the store partition"; exit 1
fi
echo "RUNWAY-GUEST: store partition verified"

# systemd-gpt-auto-generator makes no unit for the store, with the guest fstab and with none
for tab in /etc/fstab /dev/null; do
    rm -rf /run/gen; mkdir -p /run/gen/early /run/gen/normal /run/gen/late
    echo "RUNWAY-GUEST: gpt-auto-generator, fstab $tab"
    SYSTEMD_FSTAB=$tab SYSTEMD_LOG_TARGET=console SYSTEMD_LOG_LEVEL=debug \
        /usr/lib/systemd/system-generators/systemd-gpt-auto-generator /run/gen/early /run/gen/normal /run/gen/late 2>&1 \
        || { echo "FAIL: gpt-auto-generator failed"; exit 1; }
    find /run/gen -type f | sort
    if [ "$STORE" = block ] && grep -r -e sda2 -e "$STORE_PARTUUID" -e "$STORE_DEV" /run/gen; then
        echo "FAIL: gpt-auto-generator made a unit for the store partition"; exit 1
    fi
done
rm -rf /run/gen
echo "RUNWAY-GUEST: gpt-auto-generator verified"

# The packaged boot.mount finds the store alone: no drop-in, and the ESP is not needed for a store partition
systemctl cat boot.mount | grep -q '^What=auto$' || { echo "FAIL: boot.mount does not use auto"; exit 1; }
[ ! -e /etc/systemd/system/boot.mount.d ] || { echo "FAIL: a drop-in overrides boot.mount"; exit 1; }
[ "$(rwyctl members)" = "$(readlink -f "$STORE_DEV")" ] || { rwyctl -v members; echo "FAIL: rwyctl members does not name the store"; exit 1; }
if [ "$STORE" = block ]; then
    if systemctl show -p Requires -p RequiresMountsFor -p BindsTo boot.mount | grep -e /efi -e efi.mount; then
        echo "FAIL: boot.mount needs the ESP"; exit 1
    fi
    # A unit that closes the device policy reaches the store only with DeviceAllow= (the guest has no bus for systemd-run)
    dev=$(readlink -f "$STORE_DEV")
    for allow in "DeviceAllow=$dev rw" ""; do
        printf '[Service]\nType=oneshot\nDevicePolicy=closed\n%s\nStandardOutput=file:/run/hard.out\nStandardError=file:/run/hard.out\nExecStart=/usr/bin/rwyctl --store %s status\n' \
            "$allow" "$dev" > /run/systemd/system/rwy-hard.service
        systemctl daemon-reload
        if systemctl start rwy-hard.service; then got=yes; else got=no; fi
        if [ -n "$allow" ] && [ "$got" = no ]; then cat /run/hard.out; echo "FAIL: DeviceAllow= did not give access to $dev"; exit 1; fi
        if [ -z "$allow" ] && [ "$got" = yes ]; then echo "FAIL: a closed device policy still reached $dev"; exit 1; fi
    done
    rm -f /run/systemd/system/rwy-hard.service /run/hard.out
    systemctl daemon-reload
fi
# As at boot, /boot and /efi are empty: with no fstab line gpt-auto mounts the ESP on /efi and leaves /boot alone
rm -rf /run/gen; mkdir -p /run/gen/early /run/gen/normal /run/gen/late
unshare -m --propagation private sh -c 'umount /boot && umount /efi && SYSTEMD_FSTAB=/dev/null SYSTEMD_LOG_TARGET=console SYSTEMD_LOG_LEVEL=debug \
    /usr/lib/systemd/system-generators/systemd-gpt-auto-generator /run/gen/early /run/gen/normal /run/gen/late' 2>&1 \
    || { echo "FAIL: gpt-auto-generator with an empty /boot failed"; exit 1; }
find /run/gen -type f | sort
[ -z "$(find /run/gen -name boot.mount)" ] || { echo "FAIL: gpt-auto-generator made a boot.mount"; exit 1; }
[ -n "$(find /run/gen -name efi.mount)" ] || { echo "FAIL: gpt-auto-generator made no efi.mount"; exit 1; }
rm -rf /run/gen
echo "RUNWAY-GUEST: automount verified"

# Verify Debian package installation via dpkg
dpkg -i /var/cache/runway-pkgs/*.deb || { echo "FAIL: dpkg -i failed"; exit 1; }
dpkg -l runway-tools runway-fuse runway-bls | grep -E '^ii' || { echo "FAIL: packages not in dpkg status"; exit 1; }
echo "RUNWAY-GUEST: packages verified"

# bootctl status and list
bootctl --esp-path=/efi --boot-path=/boot status || true
bootctl --esp-path=/efi --boot-path=/boot list | grep -q "debian-guest" || { echo "FAIL: bootctl list missing debian-guest"; exit 1; }
bootctl --esp-path=/efi --boot-path=/boot list | grep -q "not reported" && { echo "FAIL: bootctl list has entries the loader did not report"; exit 1; }
echo "RUNWAY-GUEST: bootctl list verified"

# bootctl finds the ESP and the mounted store alone, and the ESP alone when /boot is not mounted
bootctl status > /run/bootctl.out 2>&1 || { cat /run/bootctl.out; echo "FAIL: bootctl status failed"; exit 1; }
grep -q 'ESP: /efi' /run/bootctl.out && grep -q '\$BOOT: /boot' /run/bootctl.out || { cat /run/bootctl.out; echo "FAIL: bootctl status missed the ESP or /boot"; exit 1; }
unshare -m --propagation private sh -c 'umount /boot && bootctl status' > /run/bootctl.out 2>&1 || { cat /run/bootctl.out; echo "FAIL: bootctl status without /boot failed"; exit 1; }
grep -q 'ESP: /efi' /run/bootctl.out || { cat /run/bootctl.out; echo "FAIL: bootctl status without /boot missed the ESP"; exit 1; }
rm -f /run/bootctl.out
echo "RUNWAY-GUEST: bootctl status verified"

# verify systemd-bless-boot marked entry good
# (a boot of a slot that is already OK has no counter, and the service does not run)
if [ -e /sys/firmware/efi/efivars/LoaderBootCountPath-4a67b082-0a4c-41cf-b6c7-440b29bb8c4f ]; then
    systemctl is-active --quiet systemd-bless-boot.service || { systemctl status systemd-bless-boot.service; echo "FAIL: systemd-bless-boot did not run"; exit 1; }
    journalctl -b -u systemd-bless-boot.service --no-pager | grep -q "Marked boot as 'good'" || { echo "FAIL: systemd-bless-boot did not mark the boot good"; exit 1; }
fi
rwyctl --store "$STORE_DEV" status | grep -E -q "CONFIG1 +[0-9]+ +OK" || { echo "FAIL: CONFIG1 not OK in the store"; exit 1; }
[ -f /boot/loader/entries/debian-guest.conf ] || { echo "FAIL: debian-guest.conf not found after good bless"; exit 1; }
[ ! -f /boot/loader/entries/debian-guest+2.conf ] || { echo "FAIL: debian-guest+2.conf still exists"; exit 1; }
echo "RUNWAY-GUEST: bless good verified"


# kernel-install add Type 1
K_SRC="/boot/debian-guest/linux"
I_SRC="/boot/debian-guest/initrd"
[ -f "$K_SRC" ] && [ -f "$I_SRC" ] || { echo "FAIL: missing payload source files in /boot"; exit 1; }
kernel-install --verbose add 6.12-test1 "$K_SRC" "$I_SRC" || { echo "FAIL: kernel-install add failed"; journalctl -b --no-pager | tail -n 80; exit 1; }
CONF=$(ls /boot/loader/entries/*6.12-test1*.conf 2>/dev/null | head -n 1)

[ -n "$CONF" ] && [ -f "$CONF" ] || { echo "FAIL: kernel-install did not create entry in /boot/loader/entries"; exit 1; }
echo "RUNWAY-GUEST: kernel-install add Type 1 verified"

# bless rename bad
STEM=$(basename "$CONF")
STEM="${STEM%+*}"
BAD_CONF="/boot/loader/entries/${STEM}+0.conf"
mv "$CONF" "$BAD_CONF" || { echo "FAIL: bless rename bad failed"; journalctl -b --no-pager | tail -n 80; exit 1; }
[ -f "$BAD_CONF" ] || { echo "FAIL: bad entry not found after rename"; exit 1; }
if mv "$BAD_CONF" "/boot/loader/entries/${STEM}+1.conf" 2>/dev/null; then
    echo "FAIL: indeterminate rename must be rejected"; exit 1;
fi
echo "RUNWAY-GUEST: bless bad verified"

# kernel-install remove
kernel-install remove 6.12-test1 || { echo "FAIL: kernel-install remove failed"; exit 1; }
ls /boot/loader/entries/*6.12-test1* 2>/dev/null && { echo "FAIL: entry still exists after remove"; exit 1; }
echo "RUNWAY-GUEST: kernel-install remove verified"

# Type 2 UKI write, list, bless, remove
mkdir -p /boot/EFI/Linux
cp /usr/share/test-uki.efi /boot/EFI/Linux/test-uki+2.efi
[ -f /boot/EFI/Linux/test-uki+2.efi ] || { echo "FAIL: Type 2 UKI write failed"; exit 1; }
bootctl --esp-path=/efi --boot-path=/boot list | grep -q "test-uki" || { echo "FAIL: bootctl list missing test-uki"; exit 1; }
mv /boot/EFI/Linux/test-uki+2.efi /boot/EFI/Linux/test-uki.efi || { echo "FAIL: Type 2 bless failed"; exit 1; }
[ -f /boot/EFI/Linux/test-uki.efi ] || { echo "FAIL: test-uki.efi missing after bless"; exit 1; }
rm /boot/EFI/Linux/test-uki.efi || { echo "FAIL: Type 2 remove failed"; exit 1; }
echo "RUNWAY-GUEST: Type 2 UKI verified"

# Removal of booted OK slot refused
if rm /boot/loader/entries/debian-guest.conf 2>/dev/null; then
    echo "FAIL: removing booted OK slot must fail with EBUSY"; exit 1;
fi
[ -f /boot/loader/entries/debian-guest.conf ] || { echo "FAIL: booted slot was deleted"; exit 1; }
echo "RUNWAY-GUEST: removal refusal verified"

# An OS that writes into the ESP (a small one in the block layout) still fits.
# Only EFI/ is saved: RUNWAY.DAT is too big for /run in the file layout
cp -r /efi/EFI /run/esp.save
bootctl --esp-path=/efi --boot-path=/boot --no-variables install || { echo "FAIL: bootctl install failed"; exit 1; }
bootctl --esp-path=/efi --boot-path=/boot list | grep -q "debian-guest" || { echo "FAIL: bootctl list failed after install"; exit 1; }
# Put the Runway loader back: install replaced the fallback loader
rm -rf /efi/EFI && cp -r /run/esp.save /efi/EFI && rm -rf /run/esp.save && sync
echo "RUNWAY-GUEST: bootctl install verified"

echo "RUNWAY-GUEST: ALL TESTS PASSED"
poweroff -f
SVC_SCRIPT
    chmod 755 "$ODIR/usr/local/bin/runway-guest-test.sh"
fi

cat << 'SVC' > "$ODIR/usr/lib/systemd/system/runway-guest-test.service"
[Unit]
Description=Runway Guest Test Runner
After=boot.mount systemd-bless-boot.service
RequiresMountsFor=/boot /efi

[Service]
Type=oneshot
StandardOutput=journal+console
StandardError=journal+console
ExecStart=/usr/local/bin/runway-guest-test.sh

[Install]
WantedBy=multi-user.target
SVC

ln -s /usr/lib/systemd/system/runway-guest-test.service "$ODIR/etc/systemd/system/multi-user.target.wants/runway-guest-test.service"

COMBINED_TAR="$D/combined.tar"
cp -f "$ROOT_TAR" "$COMBINED_TAR"
(cd "$ODIR" && tar --owner=0 --group=0 -rf "$COMBINED_TAR" .)
rm -rf "$ODIR"

# Create rootfs ext4 image
ROOT_IMG="$D/root.img"
rm -f "$ROOT_IMG"
truncate -s ${ROOT_MB}M "$ROOT_IMG"
mkfs.ext4 -q -F -L debian-root -d "$COMBINED_TAR" "$ROOT_IMG"
rm -f "$COMBINED_TAR"

if [ "$ROOT" = luks ]; then
    resize2fs "$ROOT_IMG" $(( ROOT_MB - 16 ))M >/dev/null
    PASSPHRASE_FILE="$D/root-passphrase.txt"
    echo -n "${ROOT_PASSPHRASE:-runway-break-glass}" > "$PASSPHRASE_FILE"
    chmod 600 "$PASSPHRASE_FILE"
    cryptsetup reencrypt --encrypt --batch-mode --type luks2 --pbkdf pbkdf2 \
        --key-file "$PASSPHRASE_FILE" --reduce-device-size 16M --disable-locks \
        --uuid "$ROOT_LUKS_UUID" "$ROOT_IMG"

    SWTPM_PORT=$(python3 -c 'import socket
for p in range(20000, 30000):
    try:
        s1 = socket.socket(); s1.bind(("", p))
        s2 = socket.socket(); s2.bind(("", p + 1))
        s1.close(); s2.close()
        print(p); break
    except: pass')
    SWTPM_CTRL=$(( SWTPM_PORT + 1 ))
    swtpm socket --tpmstate dir="$TPM_DIR" \
        --server type=tcp,port="$SWTPM_PORT" \
        --ctrl type=tcp,port="$SWTPM_CTRL" \
        --tpm2 --flags not-need-init,startup-clear >/dev/null 2>&1 &
    SWTPM_PID=$!
    for i in $(seq 1 50); do
        python3 -c "import socket; s=socket.socket(); s.connect(('127.0.0.1', $SWTPM_PORT)); s.close()" 2>/dev/null && break
        sleep 0.1
    done

    PCR7_HASH="df473f69e2c57f805be51fde5edf7b254a588f3e4bb30b661a87fcb6ad8af24c"
    SYSTEMD_TPM2_DEVICE="swtpm:host=127.0.0.1,port=$SWTPM_PORT" systemd-cryptenroll \
        --unlock-key-file="$PASSPHRASE_FILE" \
        --tpm2-device=auto \
        --tpm2-pcrs="7:sha256=$PCR7_HASH" \
        --tpm2-public-key="$PCR_INITRD_PUB" \
        "$ROOT_IMG" >/dev/null

    kill $SWTPM_PID 2>/dev/null || true
    wait $SWTPM_PID 2>/dev/null || true
fi

# Assemble disk image
ESP_SECTORS=$(( ESP_MB * 1024 * 1024 / 512 ))
ROOT_SECTORS=$(( ROOT_MB * 1024 * 1024 / 512 ))
START_ESP=$START
START_ROOT=$(( START_ESP + ESP_SECTORS ))
PART_STORE=
if [ "$STORE" = block ]; then
    STORE_SECTORS=$(( STORE_MB * 1024 * 1024 / 512 ))
    PART_STORE="start=${START_ROOT}, size=${STORE_SECTORS}, type=$XBL_TYPE, uuid=$STORE_PARTUUID, name=\"STORE\"
"
    START_ROOT=$(( START_ROOT + STORE_SECTORS ))
fi
TOTAL_SECTORS=$(( START_ROOT + ROOT_SECTORS + 2048 ))

rm -f "$OUT"
truncate -s $(( TOTAL_SECTORS * 512 )) "$OUT"
sfdisk "$OUT" << PART >/dev/null
label: gpt
start=${START_ESP}, size=${ESP_SECTORS}, type=$ESP_TYPE, name="ESP"
${PART_STORE}start=${START_ROOT}, size=${ROOT_SECTORS}, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name="root"
PART

dd if="$ESP_IMG" of="$OUT" bs=512 seek=$START_ESP conv=notrunc status=none
dd if="$ROOT_IMG" of="$OUT" bs=512 seek=$START_ROOT conv=notrunc status=none
store_put "$OUT" 0 "$STORE_FILE"
rm -f "$ESP_IMG" "$ROOT_IMG" "$STORE_FILE"
echo "mkdebian: image built: $OUT"

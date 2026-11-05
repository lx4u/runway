#!/bin/sh
# tests/qemu/test_netreport.sh — The network report of an abnormal boot.
# SPDX-License-Identifier: Apache-2.0
# Two NICs, each on its own user network. The host listens on UDP for the syslog server, a pcap per NIC
# shows which NIC sent, and QMP sets the link of the second NIC before the boot runs.
#   1) a slot that cannot load, LOG-INTERFACE on the second NIC: CONDEMN is sent once, on that NIC only
#   2) the same with LOG-INTERFACE on the first NIC: sent on that NIC only
#   3) a normal boot: nothing is sent and no report line is printed
#   4) LOG-INTERFACE names no NIC: nothing is sent, and the report ends at once
#   5) link down for the whole budget: nothing is sent, and the report ends at the budget
#   6) link up 1.5 s after the boot decision: the report is still sent
#   7) store cannot be mounted: RECOVERY boots and one STORE event is sent on the second NIC
#   8) every slot FAILED: RECOVERY boots and the all_failed event is sent on the second NIC
set -e
export PATH="$PATH:/sbin:/usr/sbin"

BUILD=${BUILD:-build}
. "$(dirname "$0")/qemu_lib.sh"

IMG=$BUILD/netreport.img
ST=$BUILD/netreport.store
LOG=$BUILD/netreport.log
TIMES=$BUILD/netreport.times
GOT=$BUILD/netreport.got
STOP=$BUILD/netreport.stop
QMP=$BUILD/netreport.qmp
PCAP1=$BUILD/netreport-nic1.pcap
PCAP2=$BUILD/netreport-nic2.pcap
JUNK=$BUILD/netreport.junk
PORT=${NETREPORT_PORT:-5514}
NIC1_MAC=52:54:00:12:34:02
NIC2_MAC=52:54:00:12:34:03
BUDGET=5
T=EFI/RUNWAY

fail() { echo "FAIL: $1" >&2; [ -n "${2:-}" ] && tail -n 60 "$2" >&2; exit 1; }
expect() { grep -qF -- "$1" "$LOG" || fail "missing expected: $1" "$LOG"; }
refuse() { grep -qF -- "$1" "$LOG" && fail "unexpected: $1" "$LOG"; return 0; }
fs() { HOST_FS_SYSTEM=1 "$BUILD/host_fs" "$ST" "$@"; }

# seed IFACE BAD: a good slot 1, and a slot 2 that cannot load when BAD is 1
# log_settings IFACE: the report settings of the store in $ST, the admin NIC is IFACE
log_settings() {
    "$BUILD/rwyctl" --store "$ST" log set LOG-SERVER 10.0.3.2:$PORT
    "$BUILD/rwyctl" --store "$ST" log set LOG-INTERFACE "$1"
    "$BUILD/rwyctl" --store "$ST" log set LOG-ADDRESS 10.0.3.15/24,10.0.3.2
}

seed() {
    tests/qemu/mkdisk_multivol.sh "$IMG" 0 0 >/dev/null
    store_get "$IMG" 0 "$ST"
    fs mkdir $T
    fs mkdir $T/BLOBS
    fs put $T/BLOBS/uki.efi @"$BUILD/NATIVE_PAYLOAD.EFI"
    printf 'not a PE image' > "$JUNK"
    fs put $T/BLOBS/junk.efi @"$JUNK"
    fs mkdir $T/CONFIG1
    fs link $T/BLOBS/uki.efi $T/CONFIG1/KERNEL
    fs put $T/CONFIG1/ARGS one
    fs put $T/CONFIG1/STATE "INSTALLED
"
    fs put $T/CONFIG1/STATE "OK
"
    if [ "$2" = 1 ]; then
        fs mkdir $T/CONFIG2
        fs link $T/BLOBS/junk.efi $T/CONFIG2/KERNEL
        fs put $T/CONFIG2/ARGS two
        fs put $T/CONFIG2/STATE "INSTALLED
"
    fi
    log_settings "$1"
    store_put "$IMG" 0 "$ST"
}

# boot LINK: one boot of the disk. LINK is up, down (second NIC) or late (down, then up after "booting").
boot() {
    rm -f "$GOT" "$STOP" "$PCAP1" "$PCAP2" "$TIMES" "$QMP"
    python3 -I -c '
import socket, sys, time, os
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", int(sys.argv[2])))
s.settimeout(0.5)
out = open(sys.argv[1], "wb")
end = time.time() + 45
while time.time() < end:
    try:
        out.write(s.recvfrom(2048)[0] + b"\n")
        out.flush()
    except socket.timeout:
        if os.path.exists(sys.argv[3]): break
' "$GOT" "$PORT" "$STOP" &
    L=$!
    python3 -I -c '
import json, os, socket, sys, time
path, mode, log = sys.argv[1:4]
def booted():
    return os.path.exists(log) and b"booting" in open(log, "rb").read()
try:
    while not os.path.exists(path): time.sleep(0.05)
    s = socket.socket(socket.AF_UNIX)
    s.connect(path)
    f = s.makefile("rw")
    def cmd(execute, **args):
        f.write(json.dumps({"execute": execute, "arguments": args}) + "\n")
        f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd("qmp_capabilities")
    if mode != "up": cmd("set_link", name="n2", up=False)
    cmd("cont")
    if mode == "late":
        end = time.time() + 60
        while not booted() and time.time() < end: time.sleep(0.05)
        time.sleep(1.5)
        cmd("set_link", name="n2", up=True)
except (OSError, ValueError):
    pass
' "$QMP" "$1" "$LOG" &
    Q=$!
    qemu_vars_copy
    timeout 90 "$QEMU_BIN" $(qemu_args) -S -qmp unix:"$QMP",server=on,wait=off \
        -netdev user,id=n1,net=10.0.3.0/24 -device e1000,netdev=n1,mac=$NIC1_MAC \
        -object filter-dump,id=d1,netdev=n1,file="$PCAP1" \
        -netdev user,id=n2,net=10.0.3.0/24 -device e1000,netdev=n2,mac=$NIC2_MAC \
        -object filter-dump,id=d2,netdev=n2,file="$PCAP2" \
        -drive format=raw,file="$IMG" -display none -serial stdio \
        | python3 -I -c '
import sys, time
log = open(sys.argv[1], "wb")
times = open(sys.argv[2], "w")
for line in sys.stdin.buffer:
    log.write(line)
    log.flush()
    times.write("%.3f\n" % time.monotonic())
    times.flush()
' "$LOG" "$TIMES" || true
    touch "$STOP"
    wait $Q || true
    wait $L || true
}

# udp_sent PCAP: the number of UDP datagrams to the syslog port in a capture of one NIC
udp_sent() {
    python3 -I -c '
import struct, sys
d = open(sys.argv[1], "rb").read()
off, n = 24, 0
while off + 16 <= len(d):
    incl = struct.unpack_from("<I", d, off + 8)[0]
    f = d[off + 16:off + 16 + incl]
    off += 16 + incl
    if len(f) >= 38 and f[12:14] == b"\x08\x00" and f[23] == 17:
        ihl = (f[14] & 15) * 4
        if struct.unpack_from(">H", f, 14 + ihl + 2)[0] == int(sys.argv[2]): n += 1
print(n)
' "$1" "$PORT"
}

# counts N1 N2: the datagrams sent on the first and the second NIC
counts() {
    [ "$(udp_sent "$PCAP1")" = "$1" ] || fail "first NIC sent $(udp_sent "$PCAP1"), want $1" "$LOG"
    [ "$(udp_sent "$PCAP2")" = "$2" ] || fail "second NIC sent $(udp_sent "$PCAP2"), want $2" "$LOG"
}

# report_span: seconds from the line before the report line to the report line
report_span() {
    n=$(grep -n "RUNWAY: report:" "$LOG" | head -n 1 | cut -d: -f1)
    [ -n "$n" ] || fail "no report line" "$LOG"
    awk -v n="$n" 'NR == n - 1 { a = $1 } NR == n { printf "%.1f\n", $1 - a }' "$TIMES"
}

# within VALUE LOW HIGH: VALUE is a time span in the range
within() {
    awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(v >= lo && v <= hi) }' ||
        fail "report span $1 s, want $2 to $3 s" "$LOG"
}

echo "--- 1: condemned slot, LOG-INTERFACE on the second NIC: one report, on that NIC ---"
seed $NIC2_MAC 1
boot up
expect "RUNWAY: CONFIG2 FAILED, skipped (load_error)"
expect "RUNWAY: booting CONFIG1 (rev 1, OK)"
expect "RUNWAY: report: 1 sent"
[ "$(wc -l < "$GOT")" = 1 ] || fail "expected one datagram: $(cat "$GOT")" "$LOG"
grep -qF "CONDEMN" "$GOT" || fail "datagram is not the CONDEMN event: $(cat "$GOT")" "$LOG"
counts 0 1

echo "--- 2: condemned slot, LOG-INTERFACE on the first NIC: one report, on that NIC ---"
seed $NIC1_MAC 1
boot up
expect "RUNWAY: report: 1 sent"
[ "$(wc -l < "$GOT")" = 1 ] || fail "expected one datagram: $(cat "$GOT")" "$LOG"
counts 1 0

echo "--- 3: normal boot: no report, nothing sent ---"
seed $NIC2_MAC 0
boot up
expect "RUNWAY: booting CONFIG1 (rev 1, OK)"
refuse "report:"
[ ! -s "$GOT" ] || fail "a normal boot sent: $(cat "$GOT")" "$LOG"
counts 0 0

echo "--- 4: no NIC for LOG-INTERFACE: nothing sent, the report ends at once ---"
seed 52:54:00:99:99:99 1
boot up
expect "RUNWAY: report: no interface"
[ ! -s "$GOT" ] || fail "a wrong interface sent: $(cat "$GOT")" "$LOG"
counts 0 0
within "$(report_span)" 0 $BUDGET

echo "--- 5: link down for the whole budget: nothing sent, the report ends at the budget ---"
seed $NIC2_MAC 1
boot down
expect "RUNWAY: booting CONFIG1 (rev 1, OK)"
expect "RUNWAY: report: no link inside the budget"
[ ! -s "$GOT" ] || fail "a report went out with no link: $(cat "$GOT")" "$LOG"
counts 0 0
within "$(report_span)" $((BUDGET - 1)) $((BUDGET + 1))

echo "--- 6: link up after the boot decision: the report is still sent ---"
seed $NIC2_MAC 1
boot late
expect "RUNWAY: report: 1 sent"
[ "$(wc -l < "$GOT")" = 1 ] || fail "expected one datagram: $(cat "$GOT")" "$LOG"
counts 0 1
within "$(report_span)" 0 $BUDGET

echo "--- 7: store cannot be mounted: the recovery area boots and the STORE event is sent ---"
export BOOT_STORE_EXTRA="--recovery $BUILD/NATIVE_PAYLOAD.EFI --args area-recovery"
tests/qemu/mkdisk_multivol.sh "$IMG" 0 0 >/dev/null
unset BOOT_STORE_EXTRA
store_get "$IMG" 0 "$ST"
log_settings $NIC2_MAC
TBL=$("$BUILD/fsck.runway" "$ST" | sed -n 's/^committed: .* table@\([0-9]*\).*/\1/p')
[ -n "$TBL" ] || fail "no inode table in fsck.runway output"
printf '\377\377\377\377' | dd of="$ST" bs=1 seek=$((TBL * 4096)) conv=notrunc status=none
store_put "$IMG" 0 "$ST"
boot up
expect "RUNWAY: boot store cannot be mounted (0x"
expect "booting the recovery area"
refuse "RUNWAY: booting CONFIG"
expect "RUNWAY: report: 1 sent"
[ "$(wc -l < "$GOT")" = 1 ] || fail "expected one datagram: $(cat "$GOT")" "$LOG"
grep -qF "STORE" "$GOT" || fail "datagram is not the STORE event: $(cat "$GOT")" "$LOG"
counts 0 1
within "$(report_span)" 0 $BUDGET

echo "--- 8: every slot FAILED, RECOVERY boots: the boot event is sent on the second NIC ---"
seed $NIC2_MAC 0
store_get "$IMG" 0 "$ST"
fs put $T/CONFIG1/STATE "FAILED
"
fs mkdir $T/RECOVERY
fs link $T/BLOBS/uki.efi $T/RECOVERY/KERNEL
fs put $T/RECOVERY/ARGS confirm
store_put "$IMG" 0 "$ST"
boot up
expect "RUNWAY: no bootable config (all_failed), booting RECOVERY"
refuse "RUNWAY: booting CONFIG"
expect "RUNWAY: report: 1 sent"
[ "$(wc -l < "$GOT")" = 1 ] || fail "expected one datagram: $(cat "$GOT")" "$LOG"
grep -qF "all_failed" "$GOT" || fail "datagram is not the all_failed event: $(cat "$GOT")" "$LOG"
counts 0 1
within "$(report_span)" 0 $BUDGET

rm -f "$IMG" "$ST" "$LOG" "$TIMES" "$GOT" "$STOP" "$QMP" "$PCAP1" "$PCAP2" "$JUNK"
echo ">>> SUCCESS: report sent for an abnormal boot only, on the named interface, inside the budget"

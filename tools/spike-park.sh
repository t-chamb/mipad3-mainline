#!/bin/bash
# Park the Mi Pad 3 in LineageOS from a running spike (or from fastboot), for charging and
# normal use. The spike leaves the BCM4356 powered with mainline firmware loaded, and a
# warm reset does not power-cycle it; LineageOS's bcmdhd only raises WL_REG_ON, so put
# the chip in reset first (WL_REG_ON = GPIO31 low, BT_REG_ON = GPIO32 low).
# Then: restart (lands in fastboot, flag set by the spike kernel) -> fastboot reboot.
set -u
[ -f "$(dirname "$0")/local.env" ] && . "$(dirname "$0")/local.env"
SER=${ANDROID_SERIAL:?set ANDROID_SERIAL to the adb and fastboot serial of the tablet}
export ANDROID_SERIAL=$SER
W=/mnt/aosp/tmp/work
say() { echo "$(date +%T) $*"; }
gadget() { local d; for d in /dev/ttyACM*; do [ -e "$d" ] || continue; udevadm info -q property -n "$d" 2>/dev/null | grep -qx 'ID_VENDOR_ID=0525' && return 0; done; return 1; }
in_fastboot() { timeout 5 fastboot devices 2>/dev/null | grep -q $SER; }
wait_for() { local t=$SECONDS; until $2; do [ $((SECONDS-t)) -ge $1 ] && return 1; sleep 2; done; }

if gadget; then
  say "spike running: Wi-Fi/BT chip into reset, then restart"
  sudo -n python3 -I -u $W/spike-shell.py --wait 5 --timeout 30 \
    'devmem 0x10005418 4 0x8000; devmem 0x10005428 4 0x0001; echo "DOUT1=$(devmem 0x10005410 4) DOUT2=$(devmem 0x10005420 4)"' \
    '!cappu-helper restart || echo b > /proc/sysrq-trigger' 2>&1 | grep -E 'DOUT|sent' || { say "STOP: could not talk to the spike"; exit 1; }
fi
wait_for 90 in_fastboot || { say "STOP: tablet did not reach fastboot"; exit 1; }
say "fastboot reboot -> LineageOS"
timeout 20 fastboot reboot 2>&1 | tail -1
t=$SECONDS
until [ "$(timeout 5 adb get-state 2>/dev/null)" = device ]; do
  [ $((SECONDS-t)) -ge 240 ] && { say "STOP: LineageOS did not come up on adb within 4 minutes"; exit 1; }
  sleep 3
done
until [ "$(timeout 5 adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do
  [ $((SECONDS-t)) -ge 420 ] && { say "STOP: LineageOS on adb but boot not completed"; exit 1; }
  sleep 3
done
say "LineageOS up after $((SECONDS-t)) s"
adb shell 'dumpsys battery | grep -E "level|status|voltage"; getprop ro.boot.bootreason' | tr -d '\r'

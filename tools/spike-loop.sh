#!/bin/bash
# Hands-off spike run for the Mi Pad 3. LineageOS is not part of the loop.
#   1. Get the tablet into fastboot from wherever it is:
#        spike running (USB serial gadget) -> set LK's fastboot flag (RTC_PDN1 bit 13),
#                                             restart
#        LineageOS on adb                  -> adb reboot bootloader
#   2. Write the spike to the recovery partition from the running system (serial upload
#      from a running spike, adb from LineageOS) and boot it once with
#      "fastboot oem reboot-recovery". fastboot never carries the image.
#   3. Run a session on its USB serial shell and save the output.
# The spike is left running at the end. Its kernel sets the fastboot flag at boot, so
# its next exit (restart, panic, watchdog, long-press) lands in fastboot again; from
# there "fastboot reboot" boots LineageOS.
# usage: spike-loop.sh        env: SPIKE=<boot image>
set -u
[ -f "$(dirname "$0")/local.env" ] && . "$(dirname "$0")/local.env"
SER=${ANDROID_SERIAL:?set ANDROID_SERIAL to the adb and fastboot serial of the tablet}
export ANDROID_SERIAL=$SER
D=/mnt/aosp/mainline/mipad3-dts
W=/mnt/aosp/tmp/work
SPIKE=${SPIKE:-$D/spike/boot-spike.img}
LOG=$W/spike-loop.log
say() { echo "$(date +%T) $*" | tee -a $LOG; }
die() { say "STOP: $*"; exit 1; }
sh_spike() { sudo -n python3 -I -u $W/spike-shell.py "$@"; }

gadget() {   # true if the spike's USB serial gadget is up
  local d
  for d in /dev/ttyACM*; do
    [ -e "$d" ] || continue
    udevadm info -q property -n "$d" 2>/dev/null | grep -qx 'ID_VENDOR_ID=0525' && return 0
  done
  return 1
}
in_fastboot() { timeout 5 fastboot devices 2>/dev/null | grep -q $SER; }
on_adb() { [ "$(timeout 5 adb get-state 2>/dev/null)" = device ]; }
anywhere() { gadget || in_fastboot || on_adb; }
wait_for() {   # $1 = seconds, $2 = predicate function
  local t=$SECONDS
  until $2; do [ $((SECONDS-t)) -ge $1 ] && return 1; sleep 2; done
}

: > $LOG
say "looking for the tablet"
wait_for 300 anywhere || die "tablet not on USB (no spike gadget, fastboot or adb)"

# The image is written to the recovery partition by whatever system is running, not by
# fastboot: LK's fastboot download sometimes hangs for good in the middle of a transfer
# (2026-11-22: 1 in about 27, and then only a 15 s Power hold gets the tablet back).
# fastboot is only used for commands after that. FLASHED=1 once the partition is written.
FLASHED=0
RECOVERY_DEV=/dev/block/platform/soc/11230000.mmc/by-name/recovery
booted() { [ "$(timeout 5 adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; }
if ! gadget && in_fastboot; then
  # Nothing is running that could write the partition: go through LineageOS.
  timeout 20 fastboot getvar product >/dev/null 2>&1 \
    || die "fastboot not answering: hold Power for 15 to 20 s to restart the tablet"
  say "tablet is in fastboot: booting LineageOS so that the image goes in over adb"
  timeout 20 fastboot reboot >> $LOG 2>&1
  wait_for 300 booted || die "LineageOS did not come up after fastboot reboot"
fi
if gadget; then
  say "spike is running: uploading $(basename $SPIKE) to the recovery partition"
  if sh_spike --wait 5 --timeout 30 --upload $SPIKE recovery >> $LOG 2>&1; then
    FLASHED=1
  else
    say "upload not possible (old helper?), falling back to fastboot flash"
  fi
  say "setting the fastboot flag and restarting the spike"
  # Read-modify-write RTC_PDN1 through the PMIC wrapper (WACS2), then the RTC write
  # trigger. Images since 2026-11-22 set the flag in the kernel; this covers older ones.
  sh_spike --wait 5 --timeout 30 \
    'devmem 0x1000d0a0 4 0x70160000; v=$(devmem 0x1000d0a4 4); devmem 0x1000d0a8 4 1; devmem 0x1000d0a0 4 $((0xF0160000 | (v & 0xffff) | 0x2000)); devmem 0x1000d0a0 4 0xF01E0001; echo "RTC_PDN1 was $v"' \
    '!cappu-helper restart || echo b > /proc/sysrq-trigger' >> $LOG 2>&1 \
    || die "could not talk to the running spike"
elif on_adb && ! in_fastboot; then
  wait_for 300 booted || die "LineageOS on adb but not booted"
  say "LineageOS is up: writing $(basename $SPIKE) to the recovery partition over adb"
  adb root >> $LOG 2>&1; sleep 3
  wait_for 30 on_adb || die "adb did not come back as root"
  SZ=$(stat -c %s $SPIKE); SUM=$(sha256sum $SPIKE | cut -d' ' -f1)
  adb push $SPIKE /data/local/tmp/spike.img >> $LOG 2>&1 || die "adb push failed"
  adb shell "dd if=/data/local/tmp/spike.img of=$RECOVERY_DEV bs=1048576 2>/dev/null; sync; rm /data/local/tmp/spike.img" >> $LOG 2>&1
  GOT=$(adb shell "head -c $SZ $RECOVERY_DEV | sha256sum" | cut -d' ' -f1)
  [ "$GOT" = "$SUM" ] || die "recovery partition does not match the image after writing (got $GOT)"
  FLASHED=1
  say "written and verified; adb reboot bootloader"
  adb reboot bootloader
fi
wait_for 90 in_fastboot || die "tablet did not reach fastboot"
timeout 20 fastboot getvar product >/dev/null 2>&1 || die "fastboot not answering (reboot the tablet into fastboot)"

if [ $FLASHED = 0 ]; then
  say "flashing $(basename $SPIKE) to recovery with fastboot (may hang: see above)"
  timeout 120 fastboot flash recovery $SPIKE >> $LOG 2>&1 || die "fastboot flash recovery failed"
fi
timeout 20 fastboot oem reboot-recovery >> $LOG 2>&1
OUT=$D/spike/runs/$(date +%Y%m%d-%H%M%S); mkdir -p $OUT
say "spike booting"
wait_for 90 gadget || die "the spike's USB serial gadget did not appear within 90 s"
# init re-selects the USB-C role in its first seconds, which can make the host port
# drop and re-enumerate the gadget: let that pass before opening the tty
sleep 8
wait_for 30 gadget || die "the spike's USB serial gadget went away and did not come back"
# extra commands for this run: one per line in $W/session-extra.txt
EXTRA=()
[ -f $W/session-extra.txt ] && mapfile -t EXTRA < <(grep -v -E '^\s*(#|$)' $W/session-extra.txt)
if sh_spike --wait 30 --timeout 60 --save $OUT/shell \
     'uname -a' 'cat /proc/cmdline' 'cat /sys/devices/system/cpu/online' \
     'dmesg | grep -E "CPU[0-9]+:|SMP:|smp:|Spectre|failed to come|cappu-spike|cappu-mm: at early|Kernel panic"' \
     'ls -l /sys/fs/pstore; tail -c 12000 /sys/fs/pstore/console-ramoops-0 2>/dev/null' \
     'cat /sys/class/power_supply/bq24190-charger/uevent; cat /sys/class/power_supply/bq27520-0/uevent' \
     'cat /proc/interrupts' 'ls /sys/class/net /sys/bus/sdio/devices' 'dmesg' "${EXTRA[@]}" >> $LOG 2>&1; then
  say "SESSION-SAVED $OUT/shell (spike left running)"
else
  die "shell session failed, see $LOG"
fi

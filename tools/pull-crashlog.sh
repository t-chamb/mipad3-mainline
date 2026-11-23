#!/bin/bash
# The spike reset itself and the tablet sits in fastboot: boot LineageOS, whose kernel
# exposes the spike's console (ramoops zone at 0x9007f000) as /sys/fs/pstore/console-ramoops,
# and save it next to the run. usage: pull-crashlog.sh [RUN_DIR]
[ -f "$(dirname "$0")/local.env" ] && . "$(dirname "$0")/local.env"
export ANDROID_SERIAL=${ANDROID_SERIAL:?set ANDROID_SERIAL to the adb and fastboot serial of the tablet}
d=${1:-$(ls -td /mnt/aosp/mainline/mipad3-dts/spike/runs/*/ | head -1)}
timeout 5 fastboot devices | grep -q $ANDROID_SERIAL && timeout 20 fastboot reboot 2>&1 | tail -1
t=$SECONDS
until [ "$(timeout 5 adb get-state 2>/dev/null)" = device ]; do
  [ $((SECONDS-t)) -ge 240 ] && { echo "no adb after 240 s"; exit 1; }
  sleep 3
done
adb root >/dev/null 2>&1; sleep 3
adb shell 'cat /sys/fs/pstore/console-ramoops*' > $d/console-ramoops 2>/dev/null
strings $d/console-ramoops > $d/console.txt
echo "saved $d/console.txt ($(wc -l < $d/console.txt) lines), adb after $((SECONDS-t)) s"

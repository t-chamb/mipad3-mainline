#!/bin/bash
# Wait until the tablet is reachable again (LineageOS on adb, or a responsive fastboot),
# then run the spike loop and print the session results. For use after a forced restart.
[ -f "$(dirname "$0")/local.env" ] && . "$(dirname "$0")/local.env"
export ANDROID_SERIAL=${ANDROID_SERIAL:?set ANDROID_SERIAL to the adb and fastboot serial of the tablet}
W=/mnt/aosp/tmp/work
t=$SECONDS
until [ "$(timeout 5 adb get-state 2>/dev/null)" = device ] || timeout 8 fastboot getvar product >/dev/null 2>&1; do
  [ $((SECONDS-t)) -ge ${1:-21600} ] && { echo "tablet still not reachable after $((SECONDS-t)) s"; exit 1; }
  sleep 5
done
echo "$(date +%T) tablet reachable after $((SECONDS-t)) s"
if [ "$(timeout 5 adb get-state 2>/dev/null)" = device ]; then
  until [ "$(timeout 5 adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 3; done
  echo "$(date +%T) LineageOS booted: $(adb shell dumpsys battery | grep -E 'level' | tr -d '\r')"
fi
$W/spike-loop.sh 2>&1 | tail -6
d=$(ls -td /mnt/aosp/mainline/mipad3-dts/spike/runs/*/ | head -1); echo "RUN $d"
for n in 10 11 12 13 14; do [ -f $d/shell/$n.txt ] || continue; echo "## $n"; tail -n +2 $d/shell/$n.txt | grep -v -E 'sync_state|deprecated|dsi get 1 byte' | sed 's/\x1b\[[0-9;]*m//g' | cut -c1-330 | head -40; done

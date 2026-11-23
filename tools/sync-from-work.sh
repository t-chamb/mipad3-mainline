#!/bin/bash
# Copy the working scripts from the scratch directory into this directory, with the
# tablet's serial number taken out: the copies read it from ANDROID_SERIAL, which
# tools/local.env (not tracked) may set.
set -e
W=${CAPPU_WORK:-/mnt/aosp/tmp/work}
T=$(cd "$(dirname "$0")" && pwd)
cp -p $W/cappu-init/cappu-helper.c $W/spike-shell.py $W/mkspike-bootimg.sh $W/build-cappu.sh \
      $W/tools/fastboot-unstick.py $W/los-regs.sh $T/
for f in spike-loop.sh spike-park.sh pull-crashlog.sh wait-then-loop.sh; do
  sed -E \
    -e 's|^SER=[A-Z0-9]{16}$|[ -f "$(dirname "$0")/local.env" ] \&\& . "$(dirname "$0")/local.env"\nSER=${ANDROID_SERIAL:?set ANDROID_SERIAL to the adb and fastboot serial of the tablet}|' \
    -e 's|^export ANDROID_SERIAL=[A-Z0-9]{16}$|[ -f "$(dirname "$0")/local.env" ] \&\& . "$(dirname "$0")/local.env"\nexport ANDROID_SERIAL=${ANDROID_SERIAL:?set ANDROID_SERIAL to the adb and fastboot serial of the tablet}|' \
    $W/$f > $T/$f
  chmod +x $T/$f
done

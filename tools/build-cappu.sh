#!/bin/bash
# Build the 7.4 spike kernel for the Mi Pad 3 and a fastboot-bootable image. Never flashes.
set -e
S=/mnt/aosp/mainline/src/linux
D=/mnt/aosp/mainline/mipad3-dts
O=/mnt/aosp/mainline/build-cappu
mkdir -p $O
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
cd $S
make -s O=$O defconfig
scripts/kconfig/merge_config.sh -m -O $O $O/.config $D/cappu-spike.config >/dev/null
make -s O=$O olddefconfig
# report requested options that did not survive dependency resolution
while IFS= read -r l; do
  case "$l" in
    CONFIG_*=*) k=${l%%=*}; v=${l#*=}; grep -qx "$k=$v" $O/.config || echo "NOT APPLIED: $l (got: $(grep -E "^$k=|# $k is not set" $O/.config))";;
    "# CONFIG_"*" is not set") k=${l#\# }; k=${k%% *}; grep -q "^$k=" $O/.config && echo "NOT APPLIED: $l (got: $(grep "^$k=" $O/.config))";;
  esac
done < $D/cappu-spike.config
nice -n 10 make -s O=$O -j16 Image.gz modules 2>&1 | grep -v -E '^\s*$' | tail -20
ls -l $O/arch/arm64/boot/Image $O/arch/arm64/boot/Image.gz
echo BUILD-DONE

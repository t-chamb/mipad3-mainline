#!/bin/bash
# Pack the 7.4 spike kernel, the cappu DTB and a toybox initramfs into a boot.img for
# `fastboot boot` (RAM only, nothing is written to the device). Header fields copy the
# stock boot.img: base 0x40000000, kernel +0x80000, ramdisk +0x9000000, tags +0xe000000.
set -e
S=/mnt/aosp/mainline/src/linux
O=/mnt/aosp/mainline/build-cappu
D=/mnt/aosp/mainline/mipad3-dts
T=/mnt/aosp/tmp/work/toybox-build/toybox
OUT=$D/spike
# override with CMDLINE=... ; the default is the stage 1 command line
: "${CMDLINE:=bootopt=64S3,32N2,64N2}"  # the real one is built in (CONFIG_CMDLINE): LK keeps only 99 characters
mkdir -p $OUT/initramfs-src

# DTB from the board file
cpp -nostdinc -I $S/arch/arm64/boot/dts/mediatek -I $S/arch/arm64/boot/dts -I $S/include \
    -I $S/scripts/dtc/include-prefixes -undef -D__DTS__ -x assembler-with-cpp -P \
    $D/mt8176-xiaomi-cappu.dts -o $OUT/cappu.pp.dts
dtc -q -I dts -O dtb -o $OUT/mt8176-xiaomi-cappu.dtb $OUT/cappu.pp.dts

cat $O/arch/arm64/boot/Image.gz $OUT/mt8176-xiaomi-cappu.dtb > $OUT/Image.gz-dtb

# Rules from the stock LK's boot_linux_fdt (lk.bin at 0x41e1fd24, see README):
# - it copies the boot.img kernel to 0x45000000, takes the DTB from the LAST FDT magic
#   in it, and gunzips the rest to 0x40080000 with a 0x1c00000 (28 MB) output limit;
# - it aborts the boot if /chosen, /memory or /reserved-memory is missing, or if
#   /reserved-memory already has secos-reserved-memory or fb-reserved-memory.
python3 -I - $O/arch/arm64/boot/Image $OUT/mt8176-xiaomi-cappu.dtb $OUT/Image.gz-dtb <<'PYEOF'
import struct, subprocess, sys
img, dtb, packed = (open(f, 'rb').read() for f in sys.argv[1:4])
ok = True
def check(cond, msg):
    global ok
    print(('ok    ' if cond else 'FAIL  ') + msg)
    ok &= cond
footprint = struct.unpack('<Q', img[16:24])[0]
check(len(img) <= 0x1c00000, 'Image %d bytes <= 28 MB LK gunzip limit' % len(img))
check(0x40080000 + footprint < 0x41e00000,
      'kernel footprint ends at 0x%x, below LK at 0x41e00000' % (0x40080000 + footprint))
check(packed.rfind(b'\xd0\x0d\xfe\xed') == len(packed) - len(dtb),
      'LK finds the appended DTB (FDT magic appears once inside it)')
check(len(packed) < 0x49000000 - 0x45000000, 'kernel+DTB fit between staging and ramdisk')
src = subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', sys.argv[2]],
                     capture_output=True, text=True).stdout
for node in ('chosen {', 'reserved-memory {'):
    check('\t' + node in src, 'DTB has /' + node.split()[0])
check('\tmemory@' in src, 'DTB has /memory')
for bad in ('secos-reserved-memory', 'fb-reserved-memory'):
    check(bad not in src, 'no ' + bad + ' node (LK adds it)')
sys.exit(0 if ok else 1)
PYEOF

# initramfs: static toybox, a shell on the USB serial gadget, and a status report
# /init is cappu-helper itself: no shell in the boot path. toybox sh as PID 1 re-ran its
# script from the top every 80 ms, and an apostrophe in a comment swallowed the rest of a
# script (both 2026-11-22). See cappu-init/cappu-helper.c for what init does.
{
  echo "dir /bin 0755 0 0"
  for d in dev proc sys tmp mnt; do echo "dir /$d 0755 0 0"; done
  echo "nod /dev/console 0600 0 0 c 5 1"
  echo "file /bin/toybox $T 0755 0 0"
  for a in $(qemu-aarch64 $T 2>/dev/null || $T); do
    [ "$a" = toybox ] || echo "slink /bin/$a toybox 0777 0 0"
  done
  echo "slink /bin/sh toybox 0777 0 0"
  echo "file /init /mnt/aosp/tmp/work/cappu-init/cappu-helper 0755 0 0"
  echo "file /bin/cappu-helper /mnt/aosp/tmp/work/cappu-init/cappu-helper 0755 0 0"
  FW=/mnt/aosp/lineage-19.1/vendor/xiaomi/cappu/proprietary/etc/firmware
  for d in /lib /lib/firmware /lib/firmware/brcm; do echo "dir $d 0755 0 0"; done
  # Vulkan userspace (Alpine musl build of Mesa's PowerVR driver), if it has been fetched
  if [ -f /mnt/aosp/mainline/userspace/mk-vk-list.py ]; then
    python3 -I /mnt/aosp/mainline/userspace/mk-vk-list.py || { echo "FAIL  Vulkan userspace list" >&2; exit 1; }
  fi
  echo "dir /lib/firmware/powervr 0755 0 0"
  echo "file /lib/firmware/powervr/rogue_4.40.2.51_v1.fw /mnt/aosp/mainline/firmware/rogue_4.40.2.51_v1.fw 0644 0 0"
  echo "file /lib/firmware/brcm/brcmfmac4356-sdio.bin $FW/fw_bcmdhd.bin 0644 0 0"
  echo "file /lib/firmware/brcm/brcmfmac4356-sdio.txt $FW/bcm94356wlsagbl_gpio_hwoob.txt 0644 0 0"
  echo "file /lib/firmware/brcm/brcmfmac4356-sdio.xiaomi,cappu.txt $FW/bcm94356wlsagbl_gpio_hwoob.txt 0644 0 0"
} | sort -u > $OUT/initramfs.list
# host tool, built directly: running make here without ARCH=arm64 resyncs .config for x86
cc -O2 -o $OUT/gen_init_cpio $S/scripts/gen_init_cpio.c
$OUT/gen_init_cpio $OUT/initramfs.list | gzip -9 > $OUT/initramfs.cpio.gz

python3 /mnt/aosp/lineage-19.1/system/tools/mkbootimg/mkbootimg.py \
  --header_version 0 --pagesize 2048 --board 1479347649 \
  --base 0x40000000 --kernel_offset 0x00080000 --ramdisk_offset 0x09000000 \
  --tags_offset 0x0e000000 \
  --kernel $OUT/Image.gz-dtb --ramdisk $OUT/initramfs.cpio.gz \
  --cmdline "$CMDLINE" \
  -o $OUT/boot-spike.img
ls -l $OUT/boot-spike.img
echo "test with: fastboot boot $OUT/boot-spike.img   (RAM only; host side: screen /dev/ttyACM0)"

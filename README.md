# Xiaomi Mi Pad 3 (cappu) on mainline Linux

Bring-up notes and tools for running mainline Linux on the Mi Pad 3 (MediaTek MT8176,
MT8173 family) from its stock bootloader. The kernel changes are in
`github.com/t-chamb/linux`, branch `cappu-mainline` (v7.4-rc3 plus 16 commits).

## Status (2026-11-22)

| Area | State |
|------|-------|
| Boot | From the stock LK and BL31 as an Android boot image in the recovery partition |
| CPUs | All six (4x A53, 2x A72); no cpufreq or cpuidle yet |
| Storage | eMMC, HS400 |
| Console | LK's framebuffer until the display driver binds; USB serial gadget; pstore |
| Display | mediatek-drm, dual-link DSI, 1536x2048 at 60.3 Hz, panel power and backlight |
| GPU | PowerVR GX6250 with drm/imagination and Mesa's Vulkan driver, offscreen only so far |
| Power | bq24297 charger and bq27520 gauge; charges at about 1.4 A from a 3 A USB-C port |
| Wi-Fi | BCM4356 probes over SDIO; scan and connect not tried |
| Touch | driver probes; events not checked |
| Not yet | IOMMU for the display (needed to present GPU buffers), Bluetooth, audio, cameras, USB role switching |

## Layout

| Path | What |
|------|------|
| `mt8176-xiaomi-cappu.dts` | working copy of the board device tree (ahead of the kernel branch) |
| `cappu-spike.config` | kernel config fragment on top of the arm64 defconfig |
| `panel-nt35523-sharp.md` | the panel, as the vendor driver describes it |
| `lk-display-registers.txt`, `los-display-registers.txt` | display registers as LK hands them over, and under the stock kernel |
| `i2c-devices.txt` | I2C devices bound under the stock kernel (bus-address, name, driver) |
| `canary/` | bare-metal payloads used to find out what LK does before any kernel ran |
| `tools/` | build, pack and test-loop scripts, and `cappu-helper.c`, the init and test helper of the boot image |
| `userspace/` | scripts that assemble the Vulkan userspace of the boot image from Alpine packages, and the triangle test |

The scripts carry the paths of the machine they were written on (`/mnt/aosp/...`); treat
them as a record of the method rather than as something to run unchanged. They take the
tablet's serial number from `ANDROID_SERIAL`.

## Build and run, in short

    make ARCH=arm64 defconfig cappu_spike.config       # in the kernel branch
    tools/mkspike-bootimg.sh                            # kernel + DTB + initramfs as a boot image
    tools/spike-loop.sh                                 # write it to recovery, boot it, run a session

The image goes into the recovery partition (keep a copy of the original), and LK is asked
for a one-off recovery boot with `fastboot oem reboot-recovery`. A kernel that fails
falls back to the normal boot, and its console can be read from the next boot's pstore.

The rest of this file is the notebook: findings in the order they were made. Sections
near the top describe early plans; where a later section says otherwise, the later one
is right (for one thing, this LK has no `fastboot boot`).

## Where each board fact comes from

| Item | Value | Source |
|------|-------|--------|
| RAM | 4 GB at 0x40000000 | iomem, stock /memory |
| Carve-outs in the DTS | vcore DVFS buffer 0x5ffff000+0x2000, minirdump 0x444f0000+0x10000 | stock /reserved-memory |
| Carve-outs LK adds | secos 0xffa00000+0x600000, fb 0xfcf00000+0x2b00000 (both no-map) | LK strings; absent from the 3.18 DTS |
| pstore | stock RAM console 0x44400000+0x80000 | stock /reserved-memory |
| Framebuffer | 1536x2048, 32 bpp, stride 6144 | stock fb0 |
| PSCI | 0.1 only, IDs 0x84000001..4; CPUs use "mt-boot" | stock /psci, /cpus |
| PMIC MT6397 | interrupt EINT5, level high | stock pwrap/mt6397pmic |
| DA9212 | i2c1 0x68, BUCKA enable on PMIC GPIO24, no interrupt | Xiaomi DTS |
| eMMC | vemc_3v3 + vio18, hs200 delay 26, hs400 delay 14, stock pin pulls/drives | stock mmc@11230000 and its pin states |
| SDIO (BCM4356 Wi-Fi) | REG_ON GPIO31, HOST_WAKE GPIO30, no supplies declared, 4 mA pins | stock sdio@11260000, bcmdhd node |
| Bluetooth | UART1 (ttyMT1), REG_ON GPIO32, DEV_WAKE GPIO28, HOST_WAKE GPIO37 | stock connectivity-bt, ro.btconfig.dev |
| Touch FT5x46 | i2c0 0x38, irq GPIO104, reset PMIC GPIO12 | stock + i2c list |
| Backlight LP8556 | i2c2 0x2c, enable GPIO103 | stock |
| Panel bias TPS65132 | i2c2 0x3e, enables GPIO8/GPIO13 | stock lcm |
| Fuel gauge / charger | bq27520 0x55; bq24297 0x6b, irq EINT105 falling | stock |
| Sensors (i2c4) | ak09912 0x0c, cm3232 0x10, ktd2026 0x30, bmi160 0x68 (irq GPIO14), 3x tmp103 0x70-72 | stock + i2c list |
| Audio (i2c6) | rt5659 0x1b, tfa98xx 0x34/0x37 | stock |
| USB-C | tusb320 at 0x47 (the stock tree says 0x60), irq GPIO88 | i2c list |
| Hall sensors | GPIO116 cover (SW_LID); GPIO70 back, Xiaomi-only code 0x12 | stock gpio_keys |
| Volume keys | SoC keypad, key 1 = down, key 2 = up | stock keypad init map |

I2C controllers keep the stock numbering (`&i2cN` = same controller); on stock Linux the
bus number is N + 4.

## Validation (no device needed)

- `dtc` 1.7.2: builds. The one warning comes from `mt8173.dtsi` (hdmi ports).
- `dt-validate` (dtschema 2026.9) against the 7.4-rc3 bindings:
  - `xiaomi,cappu` is not listed in `arm/mediatek.yaml`: needs a binding patch.
  - DA9212 binding requires an interrupt; the stock board wires none (the driver runs
    without one).
  - `mmsys` assigned-clocks: comes from `mt8173.dtsi`, not this board.
- Kernel (7.4-rc3 `c44104f36`, defconfig + `cappu-spike.config`): the Image is 26.6 MB
  against LK's 28 MB limit; its footprint ends at 0x41a80000.
  `spike/boot-spike.img` (11.8 MB) passes every LK check in the packer.

## What the stock LK does with the kernel (reverse engineered)

`lk.bin` from `/mnt/aosp/migration/mipad_3_cappu/backup/` was loaded into Ghidra as ARM at
0x41e00000 (the 512-byte MTK header stripped; wrapper ELF in `/mnt/aosp/tmp/work/lk-re/`).
`boot_linux_fdt` is at 0x41e1fd24:

- LK runs at 0x41e00000-0x41eb751c (image + BSS, from its startup code); the ATF log
  agrees ("Next image address = 0x41e00000").
- It copies the boot.img kernel to a fixed staging buffer at 0x45000000.
- It finds the DTB by scanning backwards from the end of that kernel for the FDT magic,
  so it takes the last DTB appended. It does **no** board-revision selection, unlike the
  Fire HD 10's LK, which ignores DTBs without the right `version` property.
- It gunzips the rest to `kernel_addr` (must be 512 KB aligned) with a **0x1c00000
  (28 MB) output limit**. A bigger Image means "decompress kernel image fail" and a hang.
- It copies the DTB to the tags address (0x4e000000), opens it with up to 1 MB of slack,
  then sets `/memory` (DRAM ranks, `tee_reserved_mem`), `/chosen` (initrd range,
  `atag,*`, `bootargs`) and adds `secos-reserved-memory` + `fb-reserved-memory` under
  `/reserved-memory` (helper at 0x41e0447c). **Any failure here aborts the boot**, so
  `/chosen`, `/memory` and `/reserved-memory` must exist, and `/reserved-memory` must not
  already contain nodes with those two names.
- It overwrites the root `model` with "MT8173" or "MT8176" from the chip ID (cosmetic).
- It tries an `odmdtbo` overlay; this device has no such partition.
- The 207 `mediatek,*` strings in LK belong to a separate DTB embedded in LK at 0x37d90
  (model "MT8173", extracted to `build/lk-embedded.dts`), used by LK itself.

`/mnt/aosp/tmp/work/mkspike-bootimg.sh` checks all of these before it writes an image.

## Prior art: Ryan Brue's Fire HD 10 (suez, MT8173) port

The branches are fetched into `/mnt/aosp/mainline/src/linux` as `ryanabx/*`, and the board
file is copied to `ref-mt8173-amazon-suez.dts`. It boots mainline from the **stock** LK
and BL31 of the same generation, and it has already solved what this board needs:

| suez-track/workaround commit | Relevance here |
|---|---|
| `48d04343fa59` arm64: psci: let a platform power a CPU's domain after CPU_ON | SMP: the hook |
| `c56bc6cff81a` soc: mediatek: add the MT8173 CPU MTCMOS power sequences | SMP: the stock sequences, keyed on the machine compatible (add `xiaomi,cappu`) |
| `492c739bcd0a` dts: Describe the firmware as PSCI 0.1 | same choice as here |
| `d4ce646692c5` dts: disable the PSCI CPU idle state | same |
| `e986f6b6de89` soc: mediatek: mt8173: stop a display overlay left running by the bootloader | needed with DRM_MEDIATEK (stage 2) |
| `4a87648e82c7` watchdog: mediatek: Apply the driver's mode to a watchdog left running | LK-started watchdog |
| `cfd7817312d0` rtc: mt6397: provide the 32 kHz output as a clock (+ binding) | BCM4356 sleep clock |
| `5aad8eb6cec9`, `a02827d916fb` MT6397 power controller / poweroff | power-off (BL31 SYSTEM_OFF never returns) |
| `072fdf6319ef`, `828a47ddf9bc` suspend-to-RAM through the SPM | later |
| `devices/amazon-suez` `4c370bbd1ba0` ramoops "of every reboot" | ramoops at 0x90000000; recovery clears 0x44400000 |

They also found that the SMI larb driver gates the display clocks under LK's live
scanout as soon as it probes, which freezes the overlay. So stage 1 here keeps `smi_common`,
`larb0-5` and `iommu` disabled.

## Stage 1 boot spike

1. Build the kernel: `/mnt/aosp/tmp/work/build-cappu.sh` (defconfig + `cappu-spike.config`,
   other SoC platforms dropped to stay under 28 MB).
2. Pack: `/mnt/aosp/tmp/work/mkspike-bootimg.sh` writes `spike/boot-spike.img`. It writes a
   header v0 image with the stock fields: base 0x40000000, kernel 0x40080000, ramdisk
   0x49000000, tags 0x4e000000, page 2048, board "1479347649". It also builds a static
   toybox initramfs with a shell on the USB serial gadget. Keep `bootopt=64S3,32N2,64N2`
   in the command line: LK reads it to start the kernel in 64-bit mode.
3. Command line:
   `bootopt=64S3,32N2,64N2 console=ttyGS0 console=tty0 clk_ignore_unused pd_ignore_unused regulator_ignore_unused maxcpus=1 rdinit=/init loglevel=8`
4. `fastboot boot spike/boot-spike.img` (RAM only), then on the host `screen /dev/ttyACM0`.
   Expected first signs, in order: fbcon text on the panel, then a USB serial device on
   the host carrying the kernel log, then a shell, then the eMMC partitions listed.
5. If neither the panel nor USB shows anything, boot the spike again and read
   `/sys/fs/pstore`. On the display side, try format `a8b8g8r8` or the second buffer
   (offset 0xc00000).

## Open questions, in the order they will bite

1. **Secondary CPUs will not come up with PSCI alone.** The stock "mt-boot" method
   (`drivers/misc/mediatek/base/power/mt8173/mt_psci.c`) calls PSCI CPU_ON, then the
   kernel powers the core itself through the SPM MTCMOS registers
   (`spm_mtcmos_ctrl_cpu()` in `mtcmos.c`). This firmware's CPU_ON only records the entry
   point: the stock ATF log (`atf_last.txt`, from `/proc/atf_log/atf_last`) shows
   `mt_on[cluster:cpu], entry 10103c` for every bring-up, with 0x10103c being the ATF
   warm-boot entry in SRAM. The same log shows the ATF (MediaTek fork of TF-A v1.0, built
   2018-04-18) entering the kernel at EL2 at 0x40080000 with the DTB at 0x4e000000. The
   first boot uses `maxcpus=1`; the fix is the two suez commits above.
2. **Which framebuffer is on screen** and its pixel order.
3. **Watchdog**: if LK leaves it running, the driver's probe handles it. The suez fix
   `4a87648e82c7` covers the mode bits.
4. **MT8176 OPPs**: MT8176 is a higher-clocked bin of MT8173. cpufreq stays off until
   the OPP table and the DA9212 and vpca15 wiring are checked against stock `mtcpufreq`.

## Stage 2 (after a first boot)

- **Panel:** the DSI panel needs a panel-simple or NT35523 driver, plus the Sharp init
  sequence from the stock `nt35523_wqxga_dsi_vdo_sharp` LCM driver.
- **Backlight:** the LP8556 needs a fixed regulator on GPIO103 for its enable, plus the
  stock ROM register values.
- **Display stack:** DRM_MEDIATEK, with SMI/IOMMU re-enabled and the suez overlay-stop
  initcall.
- **Wi-Fi/BT:** the BT RTS/CTS pin mux; the MT6397 RTC 32 kHz clock (suez `cfd7817312d0`).
  The BCM4356 needs that clock for Wi-Fi and BT. Stock turns it on through
  `rtc_gpio_enable_32k()`, which clears RTC_CON.F32KOB.
- **Audio:** an RT5659 + TFA98xx machine driver.
- **Smaller items:** TUSB320 role switching; KTD2026 LED channels.
- **GPU:** `powervr.exp_hw_support=1` and firmware `rogue_4.40.2.51_v1.fw` from
  `../firmware/`. The suez tree also has `385943d589d6` drm/imagination: wait for the
  firmware's interrupts before stopping it.

## First boot results (2026-11-21)

7.4-rc3 boots to userspace from the recovery partition through the stock LK; the log is in
`spike/runs/20261121-123954/console.txt`. Working: eMMC (HS400, 58.2 GiB, 31 partitions),
USB device mode (g_serial enumerates as `0525:a4a7`), MT6397 PMIC with regulators, power
key and RTC, DA9212, bq27520, power domains, watchdog handover to mtk_wdt.

What it took (each of these alone stopped a boot or made it look dead):

1. **LK keeps only the first 99 characters of the boot image command line.** Everything
   after that was silently dropped: maxcpus=1, the *_ignore_unused flags, and the
   initcall blacklist (so efifb/fbcon cleared the screen). The command line is now built
   in (`CONFIG_CMDLINE` + `CMDLINE_FORCE`), and the boot image carries only `bootopt=`.
2. **LK has no `fastboot boot`.** Test from recovery: `fastboot flash recovery
   spike/boot-spike.img`, then `fastboot oem reboot-recovery`.
3. **LK leaves the watchdog off.** `cappu-spike.c` (early_initcall) arms it the stock way,
   with the external reset bit set. Without that bit the reset wedges the board.
4. **The earlycon hack is slow** (it scrolls 12 MB of uncached memory per line); it is off
   by default.

Hands-off log loop: `/mnt/aosp/tmp/work/spike-loop.sh --skip-los-setup`. It flashes the
spike to recovery and boots it. The spike reboots itself (init after 25 s, panic=5, or the
watchdog on a hang). LineageOS then boots; its boot image reserves 0x90000000 + 0xc0000 and
runs 3.18 ramoops over it. The spike's console zone (0x9007f000, 64 KB) comes back as
`/sys/fs/pstore/console-ramoops`, which the script saves in `spike/runs/<time>/`.
Validated with `canary/canary2.S`: 4096 bytes through a watchdog reset with 0 bit errors.

Open after the first boot:
- bq24297 charger: `unable to lock HW IRQ 105`. EINT105 needs a GPIO pinmux entry.
- SDIO Wi-Fi: `sdio-pwrseq` deferred, "reset control not ready".
- Display: not yet tested with the full command line. Next: simplefb (a8b8g8r8) plus fbcon.
- SMP: port the suez MTCMOS commits, and describe all six MT8176 cores (4 A53 + 2 A72).

## Display on mainline: fixed (2026-11-21)

Symptom: Mi logo, then black with the backlight on, from every spike. Found with a
display-state tracer (`cappu-spike.c` + hooks after every driver probe and initcall).
The run logs are in `spike/runs/`.

Cause, in `drivers/pmdomain/mediatek/mtk-pm-domains.c`:
1. At registration, the power controller re-runs the full power-on sequence on every
   domain, including MM, which LK leaves running its display (DIS_PWR_CON 0xd, OVL0
   enabled, DSI0 in video mode). That sequence resets MMSYS.
2. Its first probe defers (`-517`). The mfg_async domain-supply, the DA9212 BUCKB,
   is not registered yet. The deferral cleanup then powers off every domain it added,
   MM included.

Fix (bring-up patch in the 7.4 tree, not upstreamed yet): a domain that is already on at
registration is taken over. That takes only its regulator and clock references, with
no power sequence. It is also left on, references included, through a probe-deferral
cleanup. Result: LK's pipeline keeps running (frame counters advance), simplefb +
fbcon draw on the panel, and kernel text is visible. The pixel format is a8b8g8r8.

## State on 2026-11-22: SMP, Wi-Fi, charger, shell, and a loop without LineageOS

Working on the spike (7.4-rc3 + local patches), verified over its USB serial shell:

| Item | Result |
|---|---|
| CPUs | all six online at boot: CPU0-1 A53, CPU2-3 A72, CPU4-5 A53 |
| Wi-Fi | BCM4356 on SDIO (SDR104), stock firmware 7.43.184.m2.1 as `brcmfmac4356-sdio`, `wlan0` |
| Charger | bq24297 probes, interrupt on EINT105; init switches charging on |
| Fuel gauge | bq27520 (capacity, voltage, current) |
| Shell | `cappu-helper console` on ttyGS0, driven from the host by `spike-shell.py` |
| Display | kernel console on LK's framebuffer (simplefb), see above |

How each was done:
- **SMP**: the suez commits `48d04343fa59` (PSCI hook) and `c56bc6cff81a` (CPU MTCMOS
  sequences), with `xiaomi,cappu` added and the table extended to CA7 CPU2/CPU3
  (0x21c/0x26c, 0x220/0x274, status bits 11/12). The DTS adds `cpu@2`/`cpu@3`. The A72s
  must start at boot: hot-plugging one later is refused ("Detected conflict for
  capability 82 (Spectre-v2)"). A reset with all cores powered leaves nothing behind
  (SPM PWR_STATUS is 0x0600634c again at the next early_initcall).
- **Wi-Fi**: `CONFIG_RESET_GPIO=y` (pwrseq_simple with one reset GPIO needs it), cfg80211
  and brcmfmac built in, firmware and `bcm94356wlsagbl_gpio_hwoob.txt` from the stock
  vendor image in the initramfs.
- **Charger**: pinmux GPIO105 as GPIO for the interrupt; `disable-reset` keeps LK's
  setup (a register reset drops the input limit and drained the battery); LK hands over
  with charging off, so init writes `Fast` to `charge_type`. On a PC port (500 mA) the
  spike nets about +50 mA with the backlight on, +95 mA with it off (GPIO103 low).
- **Long-press reset**: `mediatek,long-press-mode = <1>` on the PMIC keys; without it
  the driver disables the reset and a stuck kernel can only be stopped by a flat battery.
- **Init helper**: `/mnt/aosp/tmp/work/cappu-init/cappu-helper.c` (static): console on a
  tty with a controlling terminal, dead-man restart, clean restart. A shell-script
  version of the tty loop failed every write with EINVAL.

### Test loop (`/mnt/aosp/tmp/work/spike-loop.sh`)

LineageOS is no longer in the loop. The spike kernel sets LK's "reboot to bootloader"
flag at every boot (RTC_PDN1 bit 13, what the vendor `rtc_mark_fast()` sets; LK clears
it when it acts on it), so every exit from the spike lands in fastboot: restart, panic,
watchdog, long-press. The loop then flashes recovery, runs
`fastboot oem reboot-recovery`, and drives a session over the serial shell; fastboot to
saved session takes about 15 s. The previous spike boot's log is in the spike's own
`/sys/fs/pstore/console-ramoops-0`. An older spike without the flag is handled by
writing the RTC bit through the PMIC wrapper with `devmem` (in the script).

To go back to LineageOS use `/mnt/aosp/tmp/work/spike-park.sh`: it puts the BCM4356 in
reset (GPIO31/GPIO32 low), restarts into fastboot and runs `fastboot reboot`.

Open: a direct reset from a spike that had Wi-Fi up into LineageOS gave a 16 s boot loop
(preloader on USB every 16.4 s, never reaching LineageOS's USB). Suspected cause: the
warm reset does not power-cycle the BCM4356 and bcmdhd only raises WL_REG_ON. Not
proven; the park script avoids the path.

## GPU on mainline: renders correctly (2026-11-22)

- **Kernel**: `CONFIG_DRM_POWERVR=y`, `powervr.exp_hw_support=1`, firmware
  `powervr/rogue_4.40.2.51_v1.fw` in the initramfs. Probe log: "Running on unknown
  hardware; expect issues", "FW version v1.1 (build 6976702 OS)", "Initialized powervr
  1.0.0". `/dev/dri/card0` and `renderD128` appear; the mfg power domains come up with
  the DA9212 BUCKB at 1.0 V.
- **Userspace**: Alpine 3.24 aarch64 packages unpacked as archives into
  `/mnt/aosp/mainline/userspace/root` (`apk-fetch.py`: no chroot, nothing from the
  packages runs on the host). `mk-vk-list.py` puts the PowerVR ICD
  (`mesa-vulkan-imagination` 26.1.6), the loader, `vulkaninfo`, `vkcube` and their ELF
  dependency closure (30 files, 21 MB) into the initramfs. The boot image is 18.7 MB.
- **Result**: with `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1`, `vulkaninfo` reports "PowerVR
  Rogue GX6250", Vulkan 1.2.354, "Imagination open-source Mesa driver".
  `userspace/tests/vk_tri.c` (cross-built against the Alpine musl and loader) renders
  a triangle offscreen at 64x64 and at 1024x1024; every sampled pixel matches the expected
  value exactly, and the 1024x1024 result is copied to `/dev/fb0`.
- **Next for on-screen Vulkan**: Mesa pairs `mediatek,mt8173-gpu` with the
  `mediatek,mt8173-mmsys` DRM device for VK_KHR_display, so that needs the real display
  driver. `lk-display-registers.txt` holds the registers of LK's running pipeline (dump
  them again with `cappu-helper dump ADDR LEN`):
  OVL0 -> COLOR0 -> AAL -> OD -> RDMA0 -> UFOE (START | LR_EN) -> SPLIT1 -> DSI0 + DSI1.

### USB power in the spike (solved 2026-11-22)

Symptom: from 16:39 on 2026-11-22 every spike session ran on battery (bq24297
`f_pg_stat=0`, `f_vbus_stat=0`, MT6397 CHRDET 0, gauge -500 to -700 mA) while USB 2.0
data worked, and LineageOS charged on the same cable.

Cause: the stock kernel's USB-C driver (`drivers/misc/type-c/ti_i2c_tusb320.c`) can
leave the TUSB320 with its CC terminations switched off. Its shutdown hook reselects UFP;
the detach interrupt that causes runs the driver's "back to DRP" sequence in the IRQ
thread, which sets `DISABLE_RD_RP` (register 0x45 bit 2), sleeps 100 ms and only then
clears it. The restart wins that race. The chip stays powered through the reset and
keeps its registers, so whatever boots next (LK, fastboot, the spike) presents no Rd, the
USB-C host port sees nothing attached and supplies no VBUS. LineageOS repairs it at its
next boot because its probe rewrites the register. Found state: `reg45=0x04`,
`regA=0x10`, both CC lines reported as an audio accessory (`reg9=0xf0`). Clearing the
bit and restarting detection gave `reg8=0x30 reg9=0xa0` (sink on a 3 A port) and VBUS
within a second.

This is a bug in the daily driver too, not only a spike problem: after a LineageOS
reboot or power-off, a USB-C to USB-C charger or host port may not supply power until
LineageOS has booted again. A-to-C cables are not affected (VBUS is always on).

Two more things were in the way of even seeing it:

- The TUSB320's bus (i2c@11013000, `/dev/i2c-4`) was dead. LK leaves GPIO34/35 (RT5659
  1.8 V and LDO enables) muxed to PCM0, so the codec is unpowered, and the unpowered
  codec holds SDA/SCL low. Either rail on frees the bus. The DTS now has both as
  always-on fixed regulators.
- An earlier note here blamed missing pull-ups after reading "pins 100/101 low" from
  0x10005260. That register is PULLSEL, not DIN. MT8173 GPIO offsets: DIR 0x000,
  PULLEN 0x100, PULLSEL 0x200, DOUT 0x400, DIN 0x500, MODE 0x600 (16 pins per register
  at 0x10 spacing, MODE 5 pins per register; SET at +4, CLR at +8).

What the spike does now (`usbc_sink()` and `charger_setup()` in cappu-helper.c, run by
init): clear `DISABLE_RD_RP`, select UFP with the `DISABLE_TERM` sequence, wait for
"attached as sink", then set the charger from the current the port advertises on CC
(3 A port: 2 A input limit, 2.048 A charge current; 1.5 A port: 1.5 A / 1.536 A). The
bq24297 alone only knows USB 2.0 detection and stays at 500 mA on a host. Measured on
the PC's USB-C port: +1.39 A into the battery with the spike running, against +0.39 A
in LineageOS with the screen off. A small guard process stops charging outside 0-45 C
by the gauge, since nothing else here watches battery temperature.

vgp4 (1.8 V) and vgp6 (3.3 V), the touch supplies, are also always-on now; with them
off the touch bus (i2c0) timed out, with them on address 0x38 NACKs (the controller is
still held in reset: next step with the display work).

For a real driver later: mainline `extcon-usbc-tusb320` has no handling of register
0x45 and no sink preference; it needs both for this board.


## Display on mainline DRM: dual-link DSI works (2026-11-22)

mediatek-drm now drives the panel itself: 1536x2048 at 60.33 Hz (stock: 16.52 ms per
frame by SurfaceFlinger's own measurement), both DSI links frame-locked to within 1 us,
a full frame through the split every frame, and the panel answering DCS reads (power
mode `0x9c`, the value the vendor's ESD check expects; DSI error count 0). fbcon runs on
it; simplefb is only the console until the DRM device binds.

Path, described as an OF graph in the DTS so that mediatek-drm builds it instead of its
fixed MT8173 path:

    OVL0 -> COLOR0 -> AAL -> OD -> RDMA0 -> UFOE (left/right mode) -> SPLIT1 -> DSI0 + DSI1

Kernel changes (working tree; a copy is in `patches/display-dual-dsi-wip.diff` and
`patches/panel-novatek-nt35523.c`):

- `mtk-mmsys`: `DDP_COMPONENT_SPLIT0/1`, MT8173 routes UFOE -> SPLIT1 -> DSI0/DSI1.
- `mtk_ddp_comp`: a SPLIT component (clock, enable); UFOE left/right mode when the
  UFOE node has `mediatek,ufoe-lr-mode` (START | LR_EN, frame size).
- `mtk_dsi`: dual link. DSI0 owns DSI1 through `mediatek,dual-dsi-slave` (DSI1's node
  stays disabled; registers, clocks and PHY are taken from it). Each link gets half of
  every line at half the pixel clock; commands go down both links; video is started
  with `DSI_DUAL_EN` on both and the start bit on the first controller only.
- `panel-novatek-nt35523`: the vendor init and sleep sequences, supplies, two reset
  lines, optional backlight; `dsi_status` in sysfs reads the panel's status registers.
- Both property names are bring-up names, not upstream bindings.

What was wrong on the way, in the order it was found. Each of these is a real fault in
mainline's handling of a display that the bootloader leaves running (or of an MT8173
DSI panel that takes DCS commands, which no upstream MT8173 board has):

1. **OVL interrupt before there is a CRTC state.** LK leaves `OVL_INTEN` set; the first
   frame-done interrupt after the vblank callback is registered dereferences a NULL
   state in `mtk_crtc_ddp_config()`. The OVL driver now masks and clears its interrupts
   before requesting the IRQ.
2. **Endless loop in the DSI interrupt handler.** It acknowledges until the engine is
   not busy; in video mode that is never. Bounded now.
3. **DSI interrupt requested after the host is registered.** Registering the host
   probes the panel; if every other component is ready, that binds the DRM device and
   runs the first modeset, panel commands included, inside `mipi_dsi_host_register()`,
   before `devm_request_irq()`. Every command then times out after 2 s although the
   hardware finished it. The IRQ is now requested first.
4. **The inherited video stream must be stopped properly.** Clearing the start bit or
   resetting the engine under LK's running stream parks the video state machine
   mid-frame, and neither the engine reset nor the block reset clears that. Symptoms:
   the first command never starts (busy, command state idle), and the two links resume
   from different lines. Then pixels only flow while both links are in their active
   region, the overlay cannot finish its frame before the next start-of-frame, and
   every other frame is dropped: 30 Hz, visible flicker, `BUF_UNDERRUN` in `DSI_STA`,
   RDMA "abnormal EOF". The driver now stops such a stream at bind time the way a
   stream is always stopped: command mode on both links, wait until both are idle,
   then drop the pairing and the start bits.
5. **Wake-up from ULPS needs the request held.** Mainline pulses `WAKEUP_EN` with no
   delay; on this controller lanes that really are in ULPS then stay there and an LP
   command hangs. Now held for 1.5 ms. (Not what bit us at boot, but reproduced by
   hand and needed after a real power-off.)

Timing. In burst mode the link, not the pixel clock, sets the frame time. The panel
mode is chosen so that mainline's formulas give the vendor's lane rate (790 Mbit/s) and
line time: 263.333 MHz, htotal 2110 for the whole panel, i.e. per link 768 active
pixels in a 1055-pixel line at 131.67 MHz. Result 16575.7 us per frame.

Tools for checking a display nobody is looking at (all in `cappu-helper`, run from the
spike shell):

| Command | What it shows |
|---|---|
| `vblank [N]` | real refresh rate from N DRM vblanks |
| `disptrace [ms]` | DSI0 frame period, DSI1's frame phase against it, pixels through the split per frame, OVL and RDMA status per frame |
| `dispcurve [ms [step [edges]]]` | where OVL, OD and the split are, sampled through a frame |
| `dsirestart FLAGS DELAY` | stop and restart the two links with a chosen sequence |
| `pattern [fb]` | test picture: blue left half, green right half, white frame, diagonal |
| `cat /sys/bus/mipi-dsi/devices/1401b000.dsi.0/dsi_status` | panel power mode, pixel format, signal mode, self-diagnostic, DSI error count |

Register references: `lk-display-registers.txt` (LK at handover), 
`los-display-registers.txt` (stock kernel with the screen on; note that it runs a
different path, RDMA0 from memory, with OVL decoupled).

Panel power and backlight (verified 2026-11-22): the DTS describes the 1.8 V enable
(GPIO71), the TPS65132 bias rails (i2c2 0x3e, enables GPIO8/GPIO13), both reset lines
(GPIO127 and PMIC GPIO14) and the LP8556 backlight (i2c2 0x2c, enable GPIO103). Three
blank/unblank cycles through `/sys/class/graphics/fb0/blank`: every rail and the reset
go low, and on the way back the panel initialises (power mode `0x9c`), the links are in
step and the rate is 60.33 Hz again.

The kernel side is on GitHub: `github.com/t-chamb/linux`, branch `cappu-mainline`
(16 commits on v7.4-rc3; `make ARCH=arm64 defconfig cappu_spike.config`). The branch
holds what has been verified on the tablet; the working tree is ahead of it.

Still open:

- The IOMMU for the display path, which Vulkan needs to scan out GPU buffers. With the
  IOMMU, SMI common and all larbs enabled the M4U registers and mediatek-drm binds, but
  the first modeset times out waiting for a vblank (no frame leaves the overlay); the
  kernel then stalls in the fbcon takeover and the hung-task detector reboots it. With
  only larb0 enabled the IOMMU driver refuses to register at all (-EINVAL: every node
  with an `iommus` property needs its larb).
- The touchscreen driver probes ("generic ft5x06 (01)") with VGP6/VGP4 as supplies;
  events not checked yet.
- OD and AAL are left as mainline configures them (dither on, AAL enabled); CMDQ unused.

### Test loop without fastboot downloads

LK's fastboot hung for good in the middle of a download on 2026-11-22 (1 transfer in
about 27; the same bug as with large images during the LineageOS work). Nothing on the
host can restart it then, only a 15 s hold of the Power key. `spike-loop.sh` therefore
no longer sends the image through fastboot: a running spike takes it over its serial
shell (`@recv`, CRC-32 checked on receipt and on read-back) and writes the recovery
partition itself; from LineageOS it is pushed over adb and written with `dd`, then
verified by SHA-256. fastboot is only used for commands (`oem reboot-recovery`,
`reboot`). `tools/fastboot-unstick.py` (in /mnt/aosp/tmp/work) confirms a hang: it
tries to feed the stalled download and reports that the bootloader is not reading.

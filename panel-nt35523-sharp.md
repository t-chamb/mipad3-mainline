# Mi Pad 3 panel: Sharp 1536x2048, NT35523 (spec for a mainline DRM panel driver)

Source: the stock LCM driver,
`/mnt/aosp/kbuild-gcc/src/drivers/misc/mediatek/lcm/nt35523_wqxga_dsi_vdo_sharp/`.

## Link

| Item | Value |
|---|---|
| Interface | **Dual DSI** (`LCM_INTERFACE_DSI_DUAL`), DSI0 + DSI1 |
| Lanes | 4 per link |
| Mode | burst video mode (`BURST_VDO_MODE`); the command-mode branch is compiled out |
| Format | RGB888, RGB order, MSB first |
| DSI PLL | `PLL_CLOCK = 395` (MHz), SSC disabled |
| Clock lane | LP per line (`clk_lp_per_line_enable = 1`) |
| Splitting | UFOE with `lr_mode_en = 1`: the frame is split left/right, one half per DSI link |
| Packet size | 256 |

## Timings (whole panel)

| | sync | back porch | front porch | active |
|---|---|---|---|---|
| Vertical | 2 | 8 | 14 | 2048 |
| Horizontal | 2 | 55 | 55 | 1536 (768 per link) |

There is also a low-power vertical front porch of 200 lines (`vertical_vfp_lp`), used by
stock for about 55 Hz when idle.

## Power sequence (stock `lcm_suspend` in reverse; GPIOs from the stock tree)

1. 1.8 V enable: GPIO71
2. VSP enable: GPIO8 (TPS65132 positive bias); VSN enable: GPIO13 (negative bias)
3. Reset: GPIO127 (`GPIO_LCD_RST_EN`) and PMIC GPIO14 (`GPIO_LCD_RST_EN_NEW`), released
   after the bias rails
4. Backlight: LP8556 on i2c2 0x2c, enabled by GPIO103

The stock suspend order is: commands below, DSI off, 140 ms, reset low, 10 ms, VSN off,
10 ms, VSP off, 10 ms, 1.8 V off.

## Init sequence (DCS, in order)

```
11                     sleep out
(delay 120 ms)
F0 55 AA 52 08 05      page 5
CE F0 1F
F0 55 AA 52 08 01      page 1
CE 06
D7 00 2F
51 FF                  brightness
55 02                  CABC still mode
53 24                  backlight control on, dimming
29                     display on
F0 55 AA 52 00 00      back to page 0
```

## Suspend sequence

```
53 00
28                     display off
(delay 100 ms)
10                     sleep in
(delay 10 ms)
```

## Mainline gaps

- Checked in 7.4-rc3: `mtk_dsi.c` has no dual-link or secondary-link support, and the UFOE
  component (`mtk_ddp_comp.c`, `mtk_ufoe_start`) only programs bypass. Both need adding:
  1. dual-DSI in mtk_dsi, with DSI0 and DSI1 configured identically (768 px per link)
     and started together;
  2. UFOE in LR mode (the MT8173 path is OVL0 -> COLOR0 -> AAL -> OD -> RDMA0 -> UFOE ->
     DSI0/DSI1).
  This is what iiowoii3389 got working on 6.12; their tree is unpublished. Stock register
  values for both can be dumped from the running LineageOS 3.18 kernel (or read after LK
  hands over: the spike tracer shows the pipeline still running then).
- The panel driver itself is small: a drm_panel with the sequences above, a dual-DSI link
  (one mipi_dsi_device per link, as in the existing dual-DSI panel drivers), and the
  GPIO and regulator sequence.
- simplefb stays as the console until DRM_MEDIATEK binds. With the suez overlay-stop
  initcall, SMI and IOMMU can be re-enabled.

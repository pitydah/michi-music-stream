# Michi Music Stream — Firmware

ESP-IDF application for the Waveshare ESP32-S3-LCD-2 board (ESP32-S3, 16 MB flash, 8 MB octal PSRAM).

## Requirements

- ESP-IDF **v5.3 LTS** (CI builds inside `espressif/idf:release-v5.3`)
- Target: `esp32s3`

## Structure

```
firmware/
├── CMakeLists.txt
├── README.md
├── partitions.csv
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild          # External peripheral pin configuration
│   └── app_main.c
└── components/
    └── michi_board/               # Board Support Package (BSP)
        ├── CMakeLists.txt
        ├── include/
        │   ├── michi_board.h      # Public BSP API
        │   └── michi_version.h    # Firmware version (single source of truth)
        └── waveshare_s3_lcd2/
            ├── board_waveshare_s3_lcd2.c
            └── font5x7.h          # Embedded 5x7 ASCII font for the boot screen
```

At boot, every subsystem that does not exist yet is logged honestly as
`subsystem=<name> state=pending phase=<N>` — no fake success.

## Build

```
idf.py set-target esp32s3
idf.py build
```

Build and flash configuration lives in `sdkconfig.defaults` (target, 16 MB flash, octal PSRAM,
custom partition table, bootloader app rollback).

External peripheral pins (DAC I2C/I2S, LED, pairing button) are configurable through
`menuconfig` under **Michi Music Stream Hardware** (`main/Kconfig.projbuild`). The defaults
target free GPIOs that do not collide with the board pinout; see
[Hardware validation pending](#hardware-validation-pending) before trusting them.

## Partitions

16 MB OTA A/B layout without factory partition (see `partitions.csv`):

| Name      | Type | Offset   | Size     |
|-----------|------|----------|----------|
| nvs       | data | 0x9000   | 24 KB    |
| otadata   | data | 0xf000   | 8 KB     |
| phy_init  | data | 0x11000  | 4 KB     |
| ota_0     | app  | 0x20000  | 4 MB     |
| ota_1     | app  | 0x420000 | 4 MB     |
| storage   | data | 0x820000 | ~7.9 MB  |

App rollback is enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), so OTA upgrades can roll
back to the previous image if the new one fails to boot.

## Hardware

### Board: Waveshare ESP32-S3-LCD-2 (no touch)

Pinout verified against the official Waveshare demo (phase 1 only brings up the display).

| Function               | GPIO | Status in phase 1              |
|------------------------|------|--------------------------------|
| LCD SCLK (SPI)         | 39   | initialized (SPI bus)          |
| LCD MOSI (SPI)         | 38   | initialized (SPI bus)          |
| LCD MISO (SPI)         | 40   | initialized (SPI bus)          |
| LCD DC                 | 42   | initialized (panel IO)         |
| LCD CS                 | 45   | initialized (panel IO)         |
| LCD RST                | -    | not wired (unused)             |
| LCD backlight          | 1    | initialized (active high, on)  |
| microSD CS             | 41   | reserved (shares LCD SPI bus)  |
| Board I2C SDA (IMU)    | 48   | reserved (QMI8658 IMU onboard; no driver in phase 1) |
| Board I2C SCL (IMU)    | 47   | reserved (QMI8658 IMU onboard; no driver in phase 1) |
| Camera (15 pins)       | 8,21,16,2,7,10,14,11,15,13,12,6,4,9,17 | reserved, unused |
| USB D-/D+              | 19,20| reserved                       |
| UART console (bridge)  | 43,44| reserved                       |
| BOOT button            | 0    | reserved                       |
| PSRAM (octal)          | 33-37| in use (PSRAM + framebuffer)   |
| Flash                 | 26-32| in use                          |
| Free GPIOs             | 3,5,18,22,23,24,25,46 | external peripherals (below) |

### External peripherals (proposal — physical validation pending)

These pins are **defaults in Kconfig**, chosen on free GPIOs with no board collision. They
are **not initialized** in phase 1 (consumers arrive in later phases) and **must be
validated on the real unit** before use — see [Hardware validation pending](#hardware-validation-pending).

| Peripheral            | Signal        | Default GPIO | Kconfig symbol          |
|-----------------------|---------------|--------------|-------------------------|
| DAC PCM5122           | I2C SDA       | 22           | `MICHI_DAC_I2C_SDA`     |
| DAC PCM5122           | I2C SCL       | 23           | `MICHI_DAC_I2C_SCL`     |
| DAC PCM5122           | I2S BCLK      | 3            | `MICHI_I2S_BCLK`        |
| DAC PCM5122           | I2S LRCK      | 18           | `MICHI_I2S_LRCK`        |
| DAC PCM5122           | I2S DIN       | 5            | `MICHI_I2S_DIN`         |
| DAC PCM5122           | I2S MCLK      | 46 (optional)| `MICHI_I2S_MCLK`        |
| LED SK6812 (U003)     | Data          | 24           | `MICHI_LED_GPIO`        |
| Pairing button        | Input (low)   | 25           | `MICHI_BUTTON_GPIO`     |

MCLK is optional: the PCM5122 has an internal PLL, so it can be left unconnected
(set `MICHI_I2S_MCLK` to `-1`).

## BSP: `components/michi_board`

The BSP owns everything specific to the board, so later phases never touch pin numbers.

What `michi_board_init()` brings up in phase 1:

- Backlight (GPIO 1, active high, on)
- SPI bus (SPI2_HOST) + ST7789 panel IO (DC=42, CS=45, 20 MHz PCLK)
- ST7789T3 panel, 240x320, 16 bpp, color data inverted (per Waveshare demo behavior)
- Framebuffer (240x320x2 = 153.6 KB) allocated **at runtime in PSRAM** — if PSRAM is
  missing or the allocation fails, the display is disabled with a clear log and the board
  keeps running degraded (no crash)

Public API (`michi_board.h`): `init`, `shutdown`, `get_info`, `get_external_pins`,
`self_test`, `display_boot_screen`, `display_clear`. The self test performs **real queries**
(`esp_chip_info`, `esp_flash_get_size`, `esp_psram_get_size`, panel/framebuffer state) and
never fakes results: `dac_present` is always `false` in phase 1 (detection lands in phase 2).
`overall` passes only when chip + flash + PSRAM + display + backlight all pass; the DAC is
**not** counted in phase 1.

The boot screen renders text rows (embedded 5x7 font, no LVGL) and reports the same
verdicts: `Flash: 16 MiB`, `PSRAM: 8 MiB`, `Display:`, `WiFi: supported`, `BLE: supported`,
`DAC: pending (phase 2)`, `Result: PASS | DEGRADED`.

## Hardware validation pending

Phase 1 assigns default pins from free GPIOs, but **no cable was measured**. Before enabling
any consumer (DAC in phase 2, LED in phase 7, button in phase 8), validate with a multimeter
in continuity mode (unit powered off):

1. **DAC I2C**: probe the DAC `SDA` line against GPIO22 and the DAC `SCL` line against
   GPIO23. Both must beep. If your DAC board was pre-wired to other pins, update
   `MICHI_DAC_I2C_SDA`/`MICHI_DAC_I2C_SCL` in `menuconfig`.
2. **DAC I2S**: probe DAC `BCLK`→GPIO3, `LRCK`→GPIO18, `DIN`→GPIO5. Each must beep.
   MCLK: only if you wire it (probe `MCLK`→GPIO46, else keep `-1`).
3. **LED**: probe the SK6812 data input → GPIO24. Must beep.
4. **Pairing button**: probe the button pin → GPIO25 and verify continuity to GND when
   pressed (active low).
5. **Display sanity (visual)**: on first boot the screen must show the boot screen with
   white text on black; if colors look inverted, flip the `esp_lcd_panel_invert_color`
   call in `board_waveshare_s3_lcd2.c`.
6. **PSRAM/Flash**: the self test logs detected sizes; a unit with different flash or PSRAM
   reports `FAIL` (expected 16 MiB / 8 MiB).

## Legacy firmware

`firmware/common/`, `firmware/standard/` and `firmware/hifi/` are **legacy prototypes, preserved
temporarily and excluded from the build**. They will be removed once their tests are migrated to
the universal firmware. The legacy `firmware/Kconfig` (Wi-Fi credentials, stream type) was
removed in phase 1 — the universal firmware defines no Wi-Fi credentials or stream type.

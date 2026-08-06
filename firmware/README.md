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
    ├── michi_board/               # Board Support Package (BSP)
    │   ├── CMakeLists.txt
    │   ├── include/
    │   │   ├── michi_board.h      # Public BSP API
    │   │   └── michi_version.h    # Firmware version (single source of truth)
    │   └── waveshare_s3_lcd2/
    │       ├── board_waveshare_s3_lcd2.c
    │       └── font5x7.h          # Embedded 5x7 ASCII font for the boot screen
    └── michi_dac/                 # DAC register + detection (phase 2)
        ├── CMakeLists.txt
        ├── include/
        │   ├── michi_dac.h        # Public DAC API
        │   └── michi_dac_types.h  # Driver ops, caps, tier, status types
        ├── dac_manager.c          # Orchestration: detect -> init -> configure
        ├── dac_registry.c         # Static driver registry
        ├── dac_classifier.c       # Caps + product tier from real evidence
        └── drivers/
            ├── pcm512x.c          # PCM5122 I2C driver (self-detectable)
            ├── pcm5102a.c         # PCM5102A I2S-only DAC (profile-bound)
            └── mock_dac.c         # CI/test driver (MICHI_DAC_MOCK, default off)
    └── michi_product_profile/     # Dynamic product profile (phase 3)
        ├── CMakeLists.txt
        ├── include/
        │   └── michi_product_profile.h  # Profile struct + API
        └── michi_product_profile.c      # Derivation from caps + board
    ├── michi_http/                # HTTP API layer (phase 4)
    │   ├── CMakeLists.txt
    │   ├── include/
    │   │   └── michi_http.h       # Handler contract + checked JSON helpers
    │   └── http_server.c          # /api/v1/receiver/info + /firmware
    ├── michi_audio_output/        # I2S pipeline, SPSC ring (phase 4)
    │   ├── CMakeLists.txt
    │   ├── Kconfig                # MICHI_AUDIO_RING_BUFFER_KB (1 MB PSRAM)
    │   ├── include/
    │   │   └── michi_audio_output.h
    │   └── michi_audio_output.c
    └── michi_volume/              # Volume 0-100 clamped (phase 4)
        ├── CMakeLists.txt
        ├── include/
        │   └── michi_volume.h
        └── michi_volume.c
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

External peripheral pins (DAC I2C/I2S, LED, pairing button) and the
`device_id` announced by the API (`MICHI_DEVICE_ID`) are configurable
through `menuconfig` under **Michi Music Stream Hardware**
(`main/Kconfig.projbuild`). The defaults target free GPIOs that do not
collide with the board pinout; see
[Hardware validation pending](#hardware-validation-pending) before trusting
them.

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
never fakes results: the DAC fields (`dac_model`/`dac_ok`) are filled by `app_main` from
`michi_dac_get_caps()` and the BSP only renders them. `overall` passes only when chip +
flash + PSRAM + display + backlight all pass; the DAC is **not** counted.

The boot screen renders text rows (embedded 5x7 font, no LVGL) and reports the same
verdicts: `Flash: 16 MiB`, `PSRAM: 8 MiB`, `Display:`, `WiFi: supported`, `BLE: supported`,
`DAC: <model> [ok|FAIL]` (or `DAC: none` when no model is known), `Result: PASS | DEGRADED`.

## DAC subsystem: `components/michi_dac`

Phase 2 implements DAC **registration and detection**. The component owns the
driver registry, the I2C bus and the NVS profile; it never fakes a detection.

### Detection model

| Source | Priority | Effect |
|--------|----------|--------|
| NVS `dac_profile` (namespace `michi_dac`, string, max 63 chars) | 1 | Force-binds the driver with the matching `board_profile` (`pcm5122`, `pcm5102a`). No probing. |
| Hardware-ID callback (`michi_dac_register_hw_id_source`) | 2 | Extension point for boards with an EEPROM carrying the DAC identity. Not implemented (no such hardware exists). |
| Autodetection | 3 | Walks the registry and probes every `self_detectable` driver in order: `pcm512x` first, then `mock` (only when `MICHI_DAC_MOCK` is enabled). |

A successful PCM5122 probe is stable, so the firmware **never writes** the
profile back; the profile is only a manual override (e.g. PCM5102A boards,
which have no control bus and therefore cannot be auto-detected).

### PCM512x detection (probe)

There is no device-ID register, so identification is honest and multi-step:

1. **ACK scan** at the real PCM512x strap addresses `0x4D`, `0x4C`, `0x4E`,
   `0x4F` (datasheet SLAS763C Table 39: prefix `10011` + ADR1/ADR2 pins).
   `0x4D` is the most common strap on commercial modules and is probed first.
   (Note: `0x4A` is **not** a PCM512x address — earlier spec drafts listed it;
   the datasheet's range is `0x4C`–`0x4F`.)
2. **Reset-default sanity**: `GPIO_EN=0x00`, `DSP_PROGRAM=0x01`,
   `DAC_ROUTING=0x11` (all verified against Linux `sound/soc/codecs/pcm512x.c`
   reg_defaults and SLAS763C). Rule: registers the firmware itself may
   legitimately modify are **excluded from the exact match** so a
   previously-configured DAC still passes — `I2S_1` is validated by mask
   (AFMT must be I2S) and accepts any ALEN the family supports (`0x00`/`0x02`/
   `0x03`), since `configure()` writes the word length.
3. **Round-trip**: write/read `ANALOG_GAIN_CTRL` (page 1) and restore the
   **original** value in every path (success, mismatch, I2C error). Any
   current value is accepted; the round-trip only proves the register is
   writable/readable, not just ACKed.

Any mismatch → `ESP_ERR_NOT_FOUND`. Real bus errors (timeout, NACK mid-
transfer) are propagated, never masked as "not found".

### Clocking: no external MCLK

The PCM5122 runs from BCLK/LRCK alone: PLL reference = BCK (`PLL_REF`),
clock divider **autoset enabled** (the reset state, `DCAS=0`), and the SCK
missing/halt detections ignored (`ERROR_DETECT = 0x18`: IDCH|IDSK, the
no-MCLK design where SCK is absent, per SLAS763C Table 82). BCK/FS/PLL
errors are **not** ignored, so `CLOCK_STATUS.CERF` still reports a wrong
BCLK/LRCK ratio or a PLL unlock. The PLL coefficients are ignored in autoset
mode, so no coefficient programming is needed at 48 kHz.

**Honest init gate**: `michi_dac_start()` polls the PLL lock flag
(`PLL_EN.PLCK`, up to 500 ms, per SLAS763C 8.3.6.3), then verifies clock
status (`CLOCK_STATUS` register), the `ERROR_DETECT` readback and the volume
readback, and only then un-mutes. Without an I2S master driving BCLK/LRCK
the PLL cannot lock, so on real hardware init **fails with
`ESP_ERR_INVALID_STATE` until phase 11 starts the I2S clocks** — the boot log
says exactly that, and `audio_available=false`. This is by design: the DAC is
never claimed as usable without evidence. **Honest limitation**: the gate
verifies the total absence of clocks, PLL lock and ratio errors via CERF; it
does **not** verify the exact BCLK/LRCK ratio relationship — that is
validated in phase 11 with the I2S frame.

### NVS profile

`dac_profile` (string, max 63 chars) under the `michi_dac` namespace:

- empty / absent → autodetection
- `pcm5122` → force-bind the PCM512x driver
- `pcm5102a` → force-bind the PCM5102A driver (the only way without a
  hardware-ID source)
- unknown value → warned about and falls back to autodetection

A profile bind is an assertion, **not** verification: `board_verified=false`
(profile-asserted, unverified). Only a real I2C probe sets `board_verified`.
The bus runs at 100 kHz by default (`MICHI_DAC_I2C_SPEED_HZ`); raise it to
400 kHz only after measuring external pull-ups (2.2–4.7 kΩ) — the internal
~45 kΩ are marginal at 400 kHz. The bus is created lazily: a profile-only
non-I2C DAC (PCM5102A) never initializes the I2C bus.

`MICHI_DAC_MOCK` (tests/CI only, default off) registers a fake DAC driver;
a mock build logs a prominent boot warning and must never be used in
production.

### Boot screen and logs

The boot screen shows `DAC: <model> [ok|FAIL]` (model/ok filled by `app_main`
from `michi_dac_get_caps()`; the BSP only renders) or `DAC: none` when no
model is known. The title is `<product_name> v<version>` — the product name
comes from the dynamic product profile, never hardcoded in the BSP. The
single consolidated profile line and the `boot=ok mode=<tier>
audio_available=<true|false>` summary are logged by `app_main`; see
[Product profile](#product-profile-componentsmichi_product_profile).

## Product profile: `components/michi_product_profile`

Phase 3 introduces the dynamic product profile: the **single source of
truth** for everything the product announces — commercial name, tier, DAC,
formats, sample rates, bit depth, connectors, volume, OTA, display, lighting
and diagnostics. Later phases (API, mDNS/BLE, screens, sessions) must read
THIS profile; no subsystem may duplicate product strings.

### Derivation (runtime, from real evidence)

`michi_product_profile_init()` derives the profile from
`michi_dac_get_caps()` + `michi_board_get_info()` +
`michi_board_self_test()` and caches it; `refresh()` re-derives without
duplicated state. Nothing is asserted: every field traces back to evidence
collected at boot.

| Field | Rule |
|-------|------|
| `tier` | `caps.tier` as degraded by `michi_dac` (HiFi/Standard drop to `diagnostic` unless detected AND initialized) — never reimplemented here |
| `product_name` | `Michi Music Stream HiFi` when tier is `hifi`, else `Michi Music Stream` |
| `audio_available` | tier is `hifi`/`standard` AND `caps.initialized` (diagnostic → always false) |
| `codecs` | always `pcm_s16le`; `pcm_s24le` added only on the HiFi tier |
| `sample_rates` | `{48000}` (system validation baseline); `max_sample_rate` is the silicon capability, exposed separately and **not** claimed as supported yet |
| `output_connector` | `differential_stereo` when `caps.differential_output`, else `single_ended_stereo`; the **physical** connector on the unit is pending hardware validation |
| `volume` | `volume_hardware` from caps; `volume_min=0`, `volume_max=100` |
| `ota_supported` | true: the partition table is OTA A/B (`ota_0`/`ota_1`); the OTA service lands in phase 13 |
| `display_present` | `self_test.display_ok`; `display_width/height` from board info |
| `lighting_status_rgb` | true (M5Stack U003 declared by the project owner; driver in phase 7) |
| `lighting_cat_contour` | **always false by project restriction**: the cat-contour LED strip is NOT implemented — no GPIO, channel or driver is reserved or announced, and the false field is the only mention |

### Examples

| Boot evidence | tier | name | audio_available |
|---------------|------|------|-----------------|
| PCM5122 detected AND initialized (PLL locked, clocks ok) | `hifi` | Michi Music Stream HiFi | true |
| PCM5102A bound by NVS profile and initialized | `standard` | Michi Music Stream | true |
| No DAC detected, or init failed (no I2S clocks yet) | `diagnostic` | Michi Music Stream | false |

At boot `app_main` logs the single consolidated key=value line
`profile: name=... tier=... audio_available=... dac=... codecs=...
sample_rates=... display=... lighting_rgb=... cat_contour=...` and the boot
summary becomes `boot=ok mode=<tier> audio_available=<true|false>` (the
phase-2 hardcoded `mode=diagnostic` is gone). The boot screen title renders
`<product_name> v<version>` from the profile.

## Phase 4 - P0 corrections

The legacy prototype audit (`firmware/common`) found 12 P0 risks. Phase 4
fixes them BY CONSTRUCTION in the new components that own them; the legacy
code is NOT touched (it is deleted during the migration). Fixes that were
already covered by earlier phases are documented as such.

### P0 table

| # | P0 risk (legacy) | Fixed by | Status |
|---|------------------|----------|--------|
| 1 | Use-after-free in `pair_confirm_handler` (cJSON pointers used after `cJSON_Delete`) | `michi_http` handler contract: copy ALL values into local buffers BEFORE delete; returning tree pointers is PROHIBITED. Phase-10 pairing handlers use this pattern | fixed-by-construction |
| 2 | Use-after-free in `session_start_post_handler` | Same handler contract (phase-12 session handlers) | fixed-by-construction |
| 3 | Partial HTTP body reads (`httpd_req_recv` once, truncated bodies) | `michi_http_read_body()`: full `Content-Length`, caller buffer IS the limit, bounded stall retries | fixed |
| 4 | No JSON type/length validation (`->valuestring`/`->valueint` on anything) | Checked helpers `michi_http_json_get_string/int/bool()`: exact type + limit, fail-not-truncate | fixed |
| 5 | Errors swallowed by `audio_output` (malloc/bind/i2s) | `michi_audio_output` propagates EVERY error (init/start/stop) and cleans up on the way out; no `ESP_ERROR_CHECK` | fixed |
| 6 | Session marked active before audio started | `michi_audio_output_start()` returns an error; the phase-12 session layer only marks the session active when start returned `ESP_OK` | fixed |
| 7 | Forced `vTaskDelete` of tasks / leaked I2S channel | Cooperative shutdown: run flag + `xTaskNotify` + `i2s_channel_disable` (unblocks in-flight writes) + join with timeout; tasks self-delete AFTER releasing resources; channel created (disabled) at init, enabled at start, DISABLED (not deleted) at stop, deleted only at deinit once no task may be alive - `start()` after `stop()` re-enables the live channel; handles NULLed | fixed |
| 8 | Unsynchronized ring buffer (legacy 4 MB, no locking) | SPSC ring: one producer (session task) / one consumer (I2S task), ALL index updates under a portMUX critical section (choice documented in `michi_audio_output.h`); replaced by 1 MB PSRAM ring (`MICHI_AUDIO_RING_BUFFER_KB`) | fixed |
| 9 | Unvalidated GPIO usage | Covered by the BSP: pins are Kconfig-driven (`Michi Music Stream Hardware` menu) and `michi_audio_output` validates every pin/rate/depth/channel before use | covered-by-phase-1 |
| 10 | NVS without recovery | Covered by `app_main` (phase 0): `ESP_ERR_NVS_NO_FREE_PAGES`/`NEW_VERSION_FOUND` → erase + retry once, halt if still broken | covered-by-phase-0 |
| 11 | 3 s blocking in the Wi-Fi event loop | Contract for `michi_wifi` (phase 9), documented below: `esp_event` handlers MUST NOT block (`vTaskDelay` is prohibited there; use `esp_timer`/queues) | fixed-by-construction (contract, phase 9) |
| 12 | Volume response != applied value | `michi_volume` clamps 0-100 and the phase-12 handler MUST answer with `michi_volume_get()` (the real applied value, including the digital-gain fallback) | fixed |

### HTTP API: `components/michi_http`

Phase 4 serves the migrated read-only endpoints on port 80, no auth (same
surface as the legacy prototype): `GET /api/v1/receiver/info` and
`GET /api/v1/receiver/firmware`. `/info` is built entirely from
`michi_product_profile_get()` (`service=michi-link`, `name`, `device_id`
from `MICHI_DEVICE_ID`, `api_version=v1-lite` for compatibility - phase 12
aligns it, `firmware{version,build_date}`, `type`=tier, `output{...}`,
`supported_codecs`, `features`); the profile gained a `build_date` field
from the single version source `michi_version.h`. `michi_http_init()`
propagates errors (boot continues, logged). Pairing (phase 10) and
session/volume (phase 12) endpoints are written with the handler contract
in `michi_http.h`: parse → copy ALL values to local buffers → delete →
process → respond; cJSON pointers never survive `cJSON_Delete` and no
macro yields a cJSON pointer.

### Audio output: `components/michi_audio_output`

I2S master (standard mode) feeding the external DAC through a 1 MB PSRAM
SPSC ring (`MICHI_AUDIO_RING_BUFFER_KB`, default 1024 - the unsynchronized
4 MB legacy ring is replaced). NOT started at boot: a phase-12 session
calls `start()` and only becomes active on `ESP_OK` (P0-6). All P0
correctness patterns live here: full error propagation (P0-5), cooperative
shutdown (P0-7), SPSC + critical sections (P0-8), pin/rate/depth
 validation (P0-9), digital volume delegated to `michi_volume_apply()` in
 the consumer task (P0-12). Startup prefill of `buffer_ms` prevents the
 first underruns.

### Volume: `components/michi_volume`

0-100, clamped at the API boundary. Hardware path: `michi_dac_set_volume()`
when the bound DAC has hardware volume and is initialized; on failure it
falls back to digital gain so the value is STILL applied. Digital path:
`michi_volume_apply()` (16-bit Q15 / 24-bit Q23 on 3-byte packed samples
- the ESP32-S3 I2S 24-bit layout - integer math, no float).
`michi_volume_get()` always returns the real
applied value - the phase-12 handler must answer with it (P0-12).

### Phase 9 contract (Wi-Fi event loop)

`michi_wifi` handlers (phase 9) MUST NEVER block: `vTaskDelay` and
blocking I/O are prohibited inside `esp_event` handlers - use `esp_timer`
or a FreeRTOS queue to defer work. This is the P0-11 fix by construction.

## Hardware validation pending

Phase 1 assigns default pins from free GPIOs, but **no cable was measured**. Before
enabling any consumer (DAC in phase 2, LED in phase 7, button in phase 8), validate
with a multimeter in continuity mode (unit powered off):

1. **DAC I2C**: probe the DAC `SDA` line against GPIO22 and the DAC `SCL` line
   against GPIO23. Both must beep. If your DAC board was pre-wired to other pins,
   update `MICHI_DAC_I2C_SDA`/`MICHI_DAC_I2C_SCL` in `menuconfig`.
2. **DAC I2C pull-ups**: measure the SDA/SCL line voltage with the unit powered
   on: both must sit near 3.3 V with the DAC connected. The bus defaults to
   **100 kHz** (`MICHI_DAC_I2C_SPEED_HZ`); internal ESP32 pull-ups (~45 kOhm)
   are enabled as a fallback but are **too weak for reliable 400 kHz
   operation** — the DAC board must provide its own pull-ups (typically
   2.2–4.7 kOhm) before raising the speed to 400 kHz.
3. **Definitive detection test**: with the firmware booted, the log lines
   `PCM512x detected at I2C 0x..` / `no DAC detected` are the ground truth. The
   firmware scans `0x4D` first, then `0x4C/0x4E/0x4F`; if your board straps ADR1/ADR2
   differently, the scan still finds it. An I2C scan of `0x4D`–`0x4F` with an
   external tool is the definitive physical test.
4. **DAC I2S**: probe DAC `BCLK`→GPIO3, `LRCK`→GPIO18, `DIN`→GPIO5. Each must beep.
   MCLK: only if you wire it (probe `MCLK`→GPIO46, else keep `-1`). The PCM5122 PLL
   makes MCLK optional; the PCM5102A has no MCLK pin at all.
5. **LED**: probe the SK6812 data input → GPIO24. Must beep.
6. **Pairing button**: probe the button pin → GPIO25 and verify continuity to GND
   when pressed (active low).
7. **Display sanity (visual)**: on first boot the screen must show the boot screen
   with white text on black; if colors look inverted, flip the
   `esp_lcd_panel_invert_color` call in `board_waveshare_s3_lcd2.c`.
8. **PSRAM/Flash**: the self test logs detected sizes; a unit with different flash
   or PSRAM reports `FAIL` (expected 16 MiB / 8 MiB).

## Legacy firmware

`firmware/common/`, `firmware/standard/` and `firmware/hifi/` are **legacy prototypes, preserved
temporarily and excluded from the build**. They will be removed once their tests are migrated to
the universal firmware. The legacy `firmware/Kconfig` (Wi-Fi credentials, stream type) was
removed in phase 1 — the universal firmware defines no Wi-Fi credentials or stream type.

/*
 * PCM512x (PCM5121/PCM5122) I2C DAC driver.
 *
 * Every register value below was verified against two authoritative sources
 * before hardcoding:
 *   [L] Linux kernel driver/header, torvalds/linux master:
 *       sound/soc/codecs/pcm512x.c and pcm512x.h
 *       (reg_defaults table, PCM512x_* defines, slave-mode init path)
 *   [D] TI datasheet SLAS763C "PCM5121, PCM5122" (Rev. C, Oct 2018), tables
 *       cited per register below.
 *
 * IMPORTANT silicon facts established while verifying:
 * - I2C slave addresses are 10011 + ADR2 + ADR1 => 7-bit 0x4C..0x4F
 *   ([D] Table 39). 0x4D is the most common strap on commercial modules
 *   ([L] DT convention). Address 0x4A does NOT exist in this family, so it
 *   is not probed (the original spec listed it; the datasheet does not).
 * - There is no device ID register; identification is ACK + readback of
 *   reset defaults + a benign register round-trip.
 * - Clock autoset (DCAS=0, [D] Table 82) is ENABLED at reset and, together
 *   with PLL reference = BCK, runs the DAC from BCLK/LRCK alone with no
 *   external MCLK. PLL coefficients are ignored in autoset mode ([D] Tables
 *   68-72 notes), so no coefficient programming is needed at 48 kHz.
 * - PLL lock flag: PLL_EN bit 4 (PLCK), 0 = locked, 1 = not locked
 *   ([D] Table 57). NOTE: the reset value of PLL_EN is 0x01 (PLLE=1) per
 *   [D] Table 57, while [L] reg_defaults lists 0x00; PLL_EN is therefore
 *   never used as a probe sanity check and is written explicitly in init.
 * - Page-1 register 0x06 is NOT a per-channel mute: it is the AMCT bit
 *   ("analog mute follows digital mute", 0 = enabled, [D] Table 124).
 *   Muting is done through MUTE (0x03) RQML|RQMR ([D] Table 56), exactly
 *   like the Linux driver (pcm512x_mute).
 * - Digital volume registers are 0x3D (left) / 0x3E (right), reset 0x30 =
 *   0.0 dB, scale -103.5 dB..0 dB in 0.5 dB steps, 0xFF = mute
 *   ([D] Tables 90/91). The addresses 0x35/0x36 in the original phase-2
 *   spec are IDAC_1/IDAC_2 (DSP clock divider), not volume; the spec value
 *   is a documented deviation.
 * - DSP_PROGRAM reset value is 0x01 (FIR interpolation, [D] Table 86);
 *   PSEL=0 is "Reserved (do not set)". The spec's "DSP program 0" is
 *   therefore implemented as the datasheet-valid default 0x01.
 */

#include <inttypes.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "michi_dac_types.h"

/* [D] Table 39: I2C slave addresses are 10011 + ADR2 + ADR1 => 0x4C..0x4F */
#define MICHI_PCM512X_ADDR_0x4C 0x4C
#define MICHI_PCM512X_ADDR_0x4D 0x4D /* most common strap on commercial modules */
#define MICHI_PCM512X_ADDR_0x4E 0x4E
#define MICHI_PCM512X_ADDR_0x4F 0x4F
#define MICHI_I2C_XFER_TIMEOUT_MS 100
#define MICHI_PCM512X_VOLUME_INITIAL 0x80 /* mid register scale, ~ -40 dB */
#define MICHI_PCM512X_PLL_POLL_ATTEMPTS 20 /* 20 x 25 ms = 500 ms total */
#define MICHI_PCM512X_PLL_POLL_DELAY_MS 25

/* Page 0 registers */
#define REG_PAGE 0x00
#define REG_RESET 0x01            /* [L] PCM512x_RESET; [D] Table 54 */
#define REG_POWER 0x02            /* [L] PCM512x_POWER; [D] Table 55 */
#define REG_MUTE 0x03             /* [L] PCM512x_MUTE; [D] Table 56 */
#define REG_PLL_EN 0x04           /* [L] PCM512x_PLL_EN; [D] Table 57 */
#define REG_GPIO_EN 0x08          /* [L] PCM512x_GPIO_EN; [D] Table 60 */
#define REG_PLL_REF 0x0D          /* [L] PCM512x_PLL_REF; [D] Table 64 */
#define REG_DAC_REF 0x0E          /* [L] PCM512x_DAC_REF; [D] Table 65 */
#define REG_ERROR_DETECT 0x25     /* [L] PCM512x_ERROR_DETECT; [D] Table 82 */
#define REG_I2S_1 0x28            /* [L] PCM512x_I2S_1; [D] Table 83 */
#define REG_DAC_ROUTING 0x2A      /* [L] PCM512x_DAC_ROUTING; [D] Table 85 */
#define REG_DSP_PROGRAM 0x2B      /* [L] PCM512x_DSP_PROGRAM; [D] Table 86 */
#define REG_DIGITAL_VOLUME_L 0x3D /* [L] PCM512x_DIGITAL_VOLUME_2; [D] Table 90 */
#define REG_DIGITAL_VOLUME_R 0x3E /* [L] PCM512x_DIGITAL_VOLUME_3; [D] Table 91 */
#define REG_CLOCK_STATUS 0x5F     /* [L] PCM512x_CLOCK_STATUS; [D] Table 108 */

/* Page 1 registers */
#define REG_P1_ANALOG_GAIN_CTRL 0x102 /* [L] PCM512x_ANALOG_GAIN_CTRL; [D] Table 122 */

/* Bit fields (all cited above) */
#define BIT_RSTM 0x10 /* [D] Table 54 */
#define BIT_RSTR 0x01
#define BIT_RQST 0x10 /* standby request, [D] Table 55 */
#define BIT_RQML 0x10 /* mute left, [D] Table 56 */
#define BIT_RQMR 0x01 /* mute right */
#define BIT_PLLE 0x01 /* [D] Table 57 */
#define BIT_PLCK 0x10 /* read-only; 0 = locked, 1 = not locked */
#define SREF_BCK 0x10 /* PLL reference = BCK, [D] Table 64 (001 << 4) */
#define BIT_IDCH 0x08 /* ignore SCK halt detection, [D] Table 82 */
#define BIT_IDSK 0x10 /* ignore SCK detection */
#define BIT_IDBK 0x20 /* ignore BCK detection */
#define BIT_IDFS 0x40 /* ignore FS detection */
#define BIT_IPLK 0x01 /* ignore PLL unlock detection */
/* No-MCLK design (SCK absent): SCK halt/missing are ignored per SLAS763C
 * Table 82 (IDCH|IDSK set). BCK/FS/PLL errors stay VISIBLE in
 * CLOCK_STATUS.CERF (IDBK/IDFS/IPLK NOT set), so a wrong BCLK/LRCK ratio or
 * a PLL unlock still fails the init gate. DCAS (0x02) stays 0: clock divider
 * autoset ENABLED. IDCM (0x04) stays 0: LRCK/BCK missing detection kept
 * active so CKMF/CERF stay honest. */
#define PCM512X_ERROR_DETECT_CFG (BIT_IDCH | BIT_IDSK)
#define ALEN_16 0x00       /* [D] Table 83 */
#define ALEN_24 0x02
#define ALEN_24_EXT 0x03   /* ALEN=3: 24-bit I2S with extra slots */
#define ALEN_MASK 0x03     /* ALEN bits 1:0 */
#define AFMT_I2S 0x00      /* bits 5:4 */
#define AFMT_MASK 0x30
#define BIT_CERF 0x01 /* clock error being reported, [D] Table 108 */
#define BIT_CKMF 0x04 /* LRCK and BCK missing */

#define PCM512X_VOLUME_MIN_REG 0x30 /* 0.0 dB ([D] Table 90) */
#define PCM512X_VOLUME_MAX_REG 0xFF /* -103.5 dB / mute */
#define PCM512X_VOLUME_STEPS ((int)(PCM512X_VOLUME_MAX_REG - PCM512X_VOLUME_MIN_REG))

static const char *TAG = "michi_dac_pcm512x";

typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t page;
    uint8_t addr;
    bool bound;
} pcm512x_ctx_t;

static pcm512x_ctx_t s_ctx;

static esp_err_t pcm512x_set_page(void *bus_ctx, uint8_t page)
{
    if (s_ctx.page == page) {
        return ESP_OK;
    }
    uint8_t buf[2] = { REG_PAGE, page };
    esp_err_t err = i2c_master_transmit(s_ctx.dev, buf, sizeof(buf),
                                        MICHI_I2C_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    s_ctx.page = page;
    return ESP_OK;
}

static esp_err_t pcm512x_reg_read(void *bus_ctx, uint16_t reg, uint8_t *val)
{
    esp_err_t err = pcm512x_set_page(bus_ctx, (uint8_t)(reg >> 8));
    if (err != ESP_OK) {
        return err;
    }
    uint8_t addr = (uint8_t)(reg & 0xFF);
    return i2c_master_transmit_receive(s_ctx.dev, &addr, 1, val, 1,
                                       MICHI_I2C_XFER_TIMEOUT_MS);
}

static esp_err_t pcm512x_reg_write(void *bus_ctx, uint16_t reg, uint8_t val)
{
    esp_err_t err = pcm512x_set_page(bus_ctx, (uint8_t)(reg >> 8));
    if (err != ESP_OK) {
        return err;
    }
    uint8_t buf[2] = { (uint8_t)(reg & 0xFF), val };
    return i2c_master_transmit(s_ctx.dev, buf, sizeof(buf),
                               MICHI_I2C_XFER_TIMEOUT_MS);
}

/* Reset-default sanity checks. Rule: registers the firmware itself may
 * legitimately modify are EXCLUDED from the exact-match (coherent with the
 * README): a previously-configured PCM512x must still pass detection.
 * - GPIO_EN / DSP_PROGRAM / DAC_ROUTING: never written by this driver, so
 *   the reset default is required exactly.
 * - I2S_1: configure() writes AFMT|ALEN, so the check validates the format
 *   bits with a mask (AFMT == I2S) and accepts every ALEN this firmware (or
 *   any other) may legitimately leave behind: 0x00/0x02/0x03 ([D] Table 83).
 * - ANALOG_GAIN_CTRL: any value is accepted; the round-trip only proves the
 *   register is writable and readable, and RESTORES the ORIGINAL value in
 *   every path (success, mismatch, I2C error). */
static esp_err_t pcm512x_sanity(void *bus_ctx)
{
    uint8_t v = 0;
    esp_err_t err = pcm512x_reg_read(bus_ctx, REG_GPIO_EN, &v); /* [D] Table 60 reset 0x00 */
    if (err != ESP_OK) {
        return err;
    }
    if (v != 0x00) {
        ESP_LOGI(TAG, "sanity fail: GPIO_EN=0x%02x (expected 0x00)", v);
        return ESP_ERR_NOT_FOUND;
    }
    err = pcm512x_reg_read(bus_ctx, REG_DSP_PROGRAM, &v); /* [D] Table 86 reset 0x01 */
    if (err != ESP_OK) {
        return err;
    }
    if (v != 0x01) {
        ESP_LOGI(TAG, "sanity fail: DSP_PROGRAM=0x%02x (expected 0x01)", v);
        return ESP_ERR_NOT_FOUND;
    }
    /* I2S_1: modified by configure(), so no exact match. Format must be I2S
     * and ALEN must be a value this family supports (16/24-bit paths). */
    err = pcm512x_reg_read(bus_ctx, REG_I2S_1, &v); /* [D] Table 83 reset 0x02 */
    if (err != ESP_OK) {
        return err;
    }
    uint8_t alen = v & ALEN_MASK;
    if ((v & AFMT_MASK) != AFMT_I2S ||
        (alen != ALEN_16 && alen != ALEN_24 && alen != ALEN_24_EXT)) {
        ESP_LOGI(TAG, "sanity fail: I2S_1=0x%02x (AFMT must be I2S, "
                 "ALEN must be 0x00/0x02/0x03)", v);
        return ESP_ERR_NOT_FOUND;
    }
    err = pcm512x_reg_read(bus_ctx, REG_DAC_ROUTING, &v); /* [D] Table 85 reset 0x11 */
    if (err != ESP_OK) {
        return err;
    }
    if (v != 0x11) {
        ESP_LOGI(TAG, "sanity fail: DAC_ROUTING=0x%02x (expected 0x11)", v);
        return ESP_ERR_NOT_FOUND;
    }
    /* Round-trip on a benign register (analog gain, page 1): any current
     * value is accepted (it may have been left by another firmware), write
     * a test value, read it back, and restore the ORIGINAL value in every
     * path. Confirms real write/read capability, not just ACK. */
    err = pcm512x_reg_read(bus_ctx, REG_P1_ANALOG_GAIN_CTRL, &v); /* [D] Table 122 reset 0x00 */
    if (err != ESP_OK) {
        return err;
    }
    uint8_t original = v;
    const uint8_t test = 0x11; /* LAGN=1, RAGN=1: -6 dB both channels */
    err = pcm512x_reg_write(bus_ctx, REG_P1_ANALOG_GAIN_CTRL, test);
    if (err != ESP_OK) {
        pcm512x_reg_write(bus_ctx, REG_P1_ANALOG_GAIN_CTRL, original);
        return err;
    }
    err = pcm512x_reg_read(bus_ctx, REG_P1_ANALOG_GAIN_CTRL, &v);
    if (err != ESP_OK) {
        pcm512x_reg_write(bus_ctx, REG_P1_ANALOG_GAIN_CTRL, original);
        return err;
    }
    if (v != test) {
        ESP_LOGI(TAG, "sanity fail: analog gain round-trip read 0x%02x (expected 0x%02x)", v, test);
        pcm512x_reg_write(bus_ctx, REG_P1_ANALOG_GAIN_CTRL, original);
        return ESP_ERR_NOT_FOUND;
    }
    return pcm512x_reg_write(bus_ctx, REG_P1_ANALOG_GAIN_CTRL, original);
}

static esp_err_t pcm512x_probe(const michi_dac_driver_t *drv, void *bus_ctx)
{
    (void)drv;
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)bus_ctx;
    esp_err_t err = ESP_ERR_NOT_FOUND;

    /* No ID register exists; identify by ACK + reset-default sanity. Try the
     * 0x4D strap first ([L] DT convention), then the other three combinations
     * ([D] Table 39). Each address gets at most 2 probe attempts; a timeout
     * (weak pull-ups) does NOT abort the scan - it logs a hint and moves on.
     * Only real non-timeout bus errors propagate. */
    const uint8_t addrs[4] = {
        MICHI_PCM512X_ADDR_0x4D,
        MICHI_PCM512X_ADDR_0x4C,
        MICHI_PCM512X_ADDR_0x4E,
        MICHI_PCM512X_ADDR_0x4F,
    };
    for (size_t i = 0; i < sizeof(addrs); i++) {
        uint8_t addr = addrs[i];
        bool acked = false;
        for (int attempt = 0; attempt < 2; attempt++) {
            esp_err_t perr = i2c_master_probe(bus, addr, MICHI_I2C_XFER_TIMEOUT_MS);
            if (perr == ESP_OK) {
                acked = true;
                break;
            }
            if (perr == ESP_ERR_NOT_FOUND) {
                break; /* no device at this address, try the next strap */
            }
            if (perr == ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "probe 0x%02x attempt %d/2 timed out: "
                         "check I2C pull-ups (2.2-4.7kOhm)", addr, attempt + 1);
                continue; /* weak/absent pull-ups: retry once, then next strap */
            }
            ESP_LOGE(TAG, "i2c_master_probe(0x%02x) failed: %s",
                     addr, esp_err_to_name(perr));
            return perr; /* real bus problem, do not mask as "not found" */
        }
        if (!acked) {
            continue;
        }
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = CONFIG_MICHI_DAC_I2C_SPEED_HZ,
        };
        err = i2c_master_bus_add_device(bus, &dev_cfg, &s_ctx.dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02x) failed: %s", addr, esp_err_to_name(err));
            return err;
        }
        s_ctx.page = 0xFF; /* force page sync on first access */
        s_ctx.addr = addr;

        err = pcm512x_sanity(bus_ctx);
        if (err == ESP_OK) {
            s_ctx.bound = true;
            ESP_LOGI(TAG, "PCM512x detected at I2C 0x%02x (ACK + reset-default sanity + round-trip)",
                     addr);
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "sanity I2C error at 0x%02x: %s", addr, esp_err_to_name(err));
            i2c_master_bus_rm_device(s_ctx.dev);
            s_ctx.dev = NULL;
            return err; /* honest: bus fault, not "no device" */
        }
        ESP_LOGI(TAG, "0x%02x ACKed but is not a PCM512x (sanity mismatch), trying next",
                 addr);
        i2c_master_bus_rm_device(s_ctx.dev);
        s_ctx.dev = NULL;
    }
    return ESP_ERR_NOT_FOUND; /* honest: no PCM512x on the bus */
}

/* Gate failure handling: mute + park in standby (best-effort) so a
 * half-configured DAC cannot pop at full scale if the I2S master is already
 * running (phase 11 flow). */
static void pcm512x_gate_cleanup(void *bus_ctx)
{
    pcm512x_reg_write(bus_ctx, REG_MUTE, BIT_RQML | BIT_RQMR);
    pcm512x_reg_write(bus_ctx, REG_POWER, BIT_RQST);
}

static esp_err_t pcm512x_init(const michi_dac_driver_t *drv, void *bus_ctx)
{
    (void)drv;
    if (!s_ctx.bound || s_ctx.dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* RSTR/RSTM must only be issued in standby ([D] Table 54), so request
     * standby first, then reset registers and modules, then configure. */
    esp_err_t err = pcm512x_reg_write(bus_ctx, REG_POWER, BIT_RQST);
    if (err != ESP_OK) {
        return err;
    }
    err = pcm512x_reg_write(bus_ctx, REG_RESET, BIT_RSTM | BIT_RSTR);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10)); /* let the reset complete ([D] auto-clear bits) */
    err = pcm512x_reg_write(bus_ctx, REG_RESET, 0x00);
    if (err != ESP_OK) {
        return err;
    }
    s_ctx.page = 0xFF; /* reset restored the page selector */

    /* Mute BEFORE configuring: if the I2S master is already running (phase
     * 11 flow), an unmuted DAC would pop at full scale during config. */
    err = pcm512x_reg_write(bus_ctx, REG_MUTE, BIT_RQML | BIT_RQMR);
    if (err != ESP_OK) {
        return err;
    }

    /* Clocking, no external MCLK needed: PLL from BCK + divider autoset.
     * PLL_EN is written explicitly because the reset value differs between
     * [D] Table 57 (PLLE=1) and [L] reg_defaults (0x00). In autoset mode the
     * PLL reference and coefficients are ignored/overridden ([D] Tables 64,
     * 68-72), so SREF=BCK documents intent and the autoset engine does the
     * rest. */
    err = pcm512x_reg_write(bus_ctx, REG_PLL_EN, BIT_PLLE);
    if (err != ESP_OK) {
        return err;
    }
    err = pcm512x_reg_write(bus_ctx, REG_PLL_REF, SREF_BCK);
    if (err != ESP_OK) {
        return err;
    }
    err = pcm512x_reg_write(bus_ctx, REG_ERROR_DETECT, PCM512X_ERROR_DETECT_CFG);
    if (err != ESP_OK) {
        return err;
    }
    err = pcm512x_reg_write(bus_ctx, REG_DAC_REF, 0x00); /* auto-select master clock ([D] Table 65) */
    if (err != ESP_OK) {
        return err;
    }

    /* I2S slave, standard format, 24-bit default (configure() narrows it). */
    err = pcm512x_reg_write(bus_ctx, REG_I2S_1, AFMT_I2S | ALEN_24);
    if (err != ESP_OK) {
        return err;
    }
    /* DSP program: datasheet-valid default 1 (FIR interpolation, [D] Table
     * 86). PSEL=0 is reserved, so the spec's "DSP program 0" maps here. */
    err = pcm512x_reg_write(bus_ctx, REG_DSP_PROGRAM, 0x01);
    if (err != ESP_OK) {
        return err;
    }

    /* Initial volume while still muted (DAC mute is active, no pops). */
    err = pcm512x_reg_write(bus_ctx, REG_DIGITAL_VOLUME_L, MICHI_PCM512X_VOLUME_INITIAL);
    if (err != ESP_OK) {
        return err;
    }
    err = pcm512x_reg_write(bus_ctx, REG_DIGITAL_VOLUME_R, MICHI_PCM512X_VOLUME_INITIAL);
    if (err != ESP_OK) {
        return err;
    }

    /* Verification round: PLL lock + clock status + config readbacks. Honest
     * gate: without running I2S clocks (BCK/LRCK) the PLL cannot lock and
     * CLOCK_STATUS reports missing clocks, so init fails with
     * ESP_ERR_INVALID_STATE - by design, until the audio subsystem (phase 11)
     * drives the I2S master. */
    bool pll_locked = false;
    for (int attempt = 0; attempt < MICHI_PCM512X_PLL_POLL_ATTEMPTS; attempt++) {
        uint8_t pll_en = 0;
        err = pcm512x_reg_read(bus_ctx, REG_PLL_EN, &pll_en);
        if (err != ESP_OK) {
            pcm512x_gate_cleanup(bus_ctx);
            return err;
        }
        if ((pll_en & BIT_PLCK) == 0) { /* [D] Table 57: 0 = locked */
            pll_locked = true;
            break;
        }
        /* SLAS763C 8.3.6.3: the PLL needs ~16 LRCK periods before the
         * auto-start, so poll instead of reading once. */
        vTaskDelay(pdMS_TO_TICKS(MICHI_PCM512X_PLL_POLL_DELAY_MS));
    }

    uint8_t clock_status = 0;
    err = pcm512x_reg_read(bus_ctx, REG_CLOCK_STATUS, &clock_status);
    if (err != ESP_OK) {
        pcm512x_gate_cleanup(bus_ctx);
        return err;
    }
    uint8_t error_detect = 0;
    err = pcm512x_reg_read(bus_ctx, REG_ERROR_DETECT, &error_detect);
    if (err != ESP_OK) {
        pcm512x_gate_cleanup(bus_ctx);
        return err;
    }
    if (error_detect != PCM512X_ERROR_DETECT_CFG) {
        pcm512x_gate_cleanup(bus_ctx);
        ESP_LOGE(TAG, "init fail: ERROR_DETECT readback 0x%02x (expected 0x%02x)",
                 error_detect, PCM512X_ERROR_DETECT_CFG);
        return ESP_ERR_INVALID_STATE;
    }

    bool clocks_present = (clock_status & (BIT_CKMF | BIT_CERF)) == 0; /* [D] Table 108 */
    if (!pll_locked || !clocks_present) {
        pcm512x_gate_cleanup(bus_ctx);
        ESP_LOGW(TAG,
                 "init fail: PLL %s, clocks %s (CLOCK_STATUS=0x%02x). "
                 "No I2S master is running yet (phase 11); start I2S before michi_dac_start().",
                 pll_locked ? "locked" : "NOT locked",
                 clocks_present ? "ok" : "MISSING/ERROR", clock_status);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t vol_l = 0, vol_r = 0;
    err = pcm512x_reg_read(bus_ctx, REG_DIGITAL_VOLUME_L, &vol_l);
    if (err != ESP_OK) {
        pcm512x_gate_cleanup(bus_ctx);
        return err;
    }
    err = pcm512x_reg_read(bus_ctx, REG_DIGITAL_VOLUME_R, &vol_r);
    if (err != ESP_OK) {
        pcm512x_gate_cleanup(bus_ctx);
        return err;
    }
    if (vol_l != MICHI_PCM512X_VOLUME_INITIAL || vol_r != MICHI_PCM512X_VOLUME_INITIAL) {
        pcm512x_gate_cleanup(bus_ctx);
        ESP_LOGE(TAG, "init fail: volume readback L=0x%02x R=0x%02x (expected 0x%02x)",
                 vol_l, vol_r, MICHI_PCM512X_VOLUME_INITIAL);
        return ESP_ERR_INVALID_STATE;
    }

    /* Gate passed: unmute LAST so the DAC never outputs during config. */
    err = pcm512x_reg_write(bus_ctx, REG_MUTE, 0x00);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "init ok: PLL locked, clocks valid, volume readback ok, unmuted");
    return ESP_OK;
}

static esp_err_t pcm512x_configure(const michi_dac_driver_t *drv, void *bus_ctx,
                                   uint32_t sample_rate, uint8_t bit_depth,
                                   uint8_t channels)
{
    (void)drv;
    /* Phase 2 validates the system at 48 kHz / 16-24 bit / 2ch only. The
     * silicon supports up to 192 kHz (caps.max_sample_rate), but that needs
     * full clock/divider validation with the I2S master in a later phase. */
    if (sample_rate != 48000 || channels != 2 ||
        (bit_depth != 16 && bit_depth != 24)) {
        ESP_LOGW(TAG, "configure unsupported: %" PRIu32 " Hz / %u bit / %u ch "
                 "(phase 2 validates 48000 Hz, 16|24 bit, 2 ch)",
                 sample_rate, bit_depth, channels);
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t alen = (bit_depth == 24) ? ALEN_24 : ALEN_16;
    esp_err_t err = pcm512x_reg_write(bus_ctx, REG_I2S_1, AFMT_I2S | alen);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t v = 0;
    err = pcm512x_reg_read(bus_ctx, REG_I2S_1, &v);
    if (err != ESP_OK) {
        return err;
    }
    if (v != (AFMT_I2S | alen)) {
        ESP_LOGE(TAG, "configure fail: I2S_1 readback 0x%02x (expected 0x%02x)", v,
                 AFMT_I2S | alen);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "configure ok: %" PRIu32 " Hz, %u bit, %u ch (I2S slave, no MCLK)",
             sample_rate, bit_depth, channels);
    return ESP_OK;
}

/* Volume: register scale is +24 dB..-103.5 dB in 0.5 dB steps, 0x30 = 0.0 dB,
 * 0xFF = mute ([D] Tables 90/91). 0-100 maps to 0.0 dB..-103.5 dB, clamped
 * so 100 never boosts above 0 dB. */
static uint8_t pcm512x_volume_to_reg(uint8_t volume_0_100)
{
    if (volume_0_100 > 100) {
        volume_0_100 = 100;
    }
    uint8_t reg = PCM512X_VOLUME_MIN_REG +
                  (uint8_t)((uint32_t)(100 - volume_0_100) * PCM512X_VOLUME_STEPS / 100);
    return reg;
}

static uint8_t pcm512x_reg_to_volume(uint8_t reg)
{
    if (reg <= PCM512X_VOLUME_MIN_REG) {
        return 100;
    }
    if (reg >= PCM512X_VOLUME_MAX_REG) {
        return 0;
    }
    return (uint8_t)(100 - (uint32_t)(reg - PCM512X_VOLUME_MIN_REG) * 100 / PCM512X_VOLUME_STEPS);
}

static esp_err_t pcm512x_set_volume(const michi_dac_driver_t *drv, void *bus_ctx,
                                    uint8_t volume_0_100)
{
    (void)drv;
    uint8_t reg = pcm512x_volume_to_reg(volume_0_100);
    esp_err_t err = pcm512x_reg_write(bus_ctx, REG_DIGITAL_VOLUME_L, reg);
    if (err != ESP_OK) {
        return err;
    }
    err = pcm512x_reg_write(bus_ctx, REG_DIGITAL_VOLUME_R, reg);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGD(TAG, "volume %u -> reg 0x%02x (~%d dB)", volume_0_100, reg,
             (int)(-103.5 + (255 - reg) * 0.5));
    return ESP_OK;
}

/* Mute via MUTE (0x03) soft mute requests ([D] Table 56). With AMCT=0 (reset
 * default, [D] Table 124) the analog outputs follow the digital mute, so this
 * mutes the whole signal path. */
static esp_err_t pcm512x_set_mute(const michi_dac_driver_t *drv, void *bus_ctx, bool mute)
{
    (void)drv;
    return pcm512x_reg_write(bus_ctx, REG_MUTE, mute ? (BIT_RQML | BIT_RQMR) : 0x00);
}

static esp_err_t pcm512x_get_status(const michi_dac_driver_t *drv, void *bus_ctx,
                                    michi_dac_status_t *status)
{
    (void)drv;
    memset(status, 0, sizeof(*status));
    status->present = s_ctx.bound;

    uint8_t pll_en = 0, clock_status = 0, mute = 0, vol_l = 0, vol_r = 0;
    esp_err_t err = pcm512x_reg_read(bus_ctx, REG_PLL_EN, &pll_en);
    if (err != ESP_OK) {
        return err;
    }
    err = pcm512x_reg_read(bus_ctx, REG_CLOCK_STATUS, &clock_status);
    if (err != ESP_OK) {
        return err;
    }
    err = pcm512x_reg_read(bus_ctx, REG_MUTE, &mute);
    if (err != ESP_OK) {
        return err;
    }
    err = pcm512x_reg_read(bus_ctx, REG_DIGITAL_VOLUME_L, &vol_l);
    if (err != ESP_OK) {
        return err;
    }
    err = pcm512x_reg_read(bus_ctx, REG_DIGITAL_VOLUME_R, &vol_r);
    if (err != ESP_OK) {
        return err;
    }

    status->i2c_ok = true;
    status->pll_locked = (pll_en & BIT_PLCK) == 0;
    status->clocks_ok = (clock_status & (BIT_CKMF | BIT_CERF)) == 0;
    status->error_flag = (clock_status & BIT_CERF) != 0;
    status->muted = (mute & (BIT_RQML | BIT_RQMR)) != 0;
    uint8_t avg_vol = (uint8_t)(((uint16_t)vol_l + vol_r) / 2);
    status->volume_0_100 = pcm512x_reg_to_volume(avg_vol);
    return ESP_OK;
}

static esp_err_t pcm512x_shutdown(const michi_dac_driver_t *drv, void *bus_ctx)
{
    (void)drv;
    esp_err_t err = pcm512x_set_mute(drv, bus_ctx, true);
    if (err != ESP_OK) {
        return err;
    }
    /* Park in standby ([D] Table 55), same as the Linux probe default. */
    err = pcm512x_reg_write(bus_ctx, REG_POWER, BIT_RQST);
    if (err != ESP_OK) {
        return err;
    }
    s_ctx.bound = false;
    ESP_LOGI(TAG, "shutdown: muted, standby requested");
    return ESP_OK;
}

/* Caps template: silicon capabilities from the PCM5122 datasheet (SNR 112 dB,
 * up to 192 kHz/24-bit/2ch, differential outputs, PLL-based no-MCLK clocking).
 * tier/detected/initialized/board_verified are filled by the classifier based
 * on real evidence, never hardcoded here. */
const michi_dac_caps_t g_michi_dac_pcm512x_caps = {
    .vendor = "TI",
    .model = "PCM5122",
    .board_profile = "pcm5122",
    .max_sample_rate = 192000, /* silicon limit ([D]); validated at 48 kHz in phase 2 */
    .max_bit_depth = 24,
    .channels = 2,
    .snr_db = 112, /* [D] datasheet SNR, 2-VRMS differential */
    .software_control = true,
    .hardware_volume = true,
    .hardware_mute = true,
    .differential_output = true,
    .headphone_output = false,
    .requires_mclk = false, /* PLL from BCK + autoset: no external MCLK needed */
    .tier = MICHI_PRODUCT_HIFI, /* classifier downgrades unless detected && initialized */
};

const michi_dac_driver_t g_michi_dac_pcm512x = {
    .name = "pcm512x",
    .board_profile = "pcm5122",
    .self_detectable = true,
    .template = &g_michi_dac_pcm512x_caps,
    .ops = {
        .probe = pcm512x_probe,
        .init = pcm512x_init,
        .configure = pcm512x_configure,
        .set_volume = pcm512x_set_volume,
        .set_mute = pcm512x_set_mute,
        .get_status = pcm512x_get_status,
        .shutdown = pcm512x_shutdown,
    },
};

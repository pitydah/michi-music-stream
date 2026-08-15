#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Semantic color palette (UI redesign, phase UI-01).
 *
 * Designed in RGB888 and converted to RGB565 with proper rounding (nearest
 * channel value, not truncation) via michi_ui_rgb888_to_rgb565(). These are
 * the ONLY color constants the renderer may use: no ad-hoc hex scattered
 * across the screens.
 *
 * The values are cross-checked by the asset generator
 * (tools/gen_michi_ui_assets.py) and by the host smoke test.
 */

#define MICHI_UI_BG                0x0842u /* #080A0F background            */
#define MICHI_UI_SURFACE           0x10A3u /* #10141A cards/panels          */
#define MICHI_UI_SURFACE_ELEVATED  0x18C4u /* #151A24 raised surfaces       */
#define MICHI_UI_TEXT_PRIMARY      0xF7BEu /* #F5F6F8 headings/values       */
#define MICHI_UI_TEXT_SECONDARY    0x9D16u /* #9AA3B3 body text             */
#define MICHI_UI_TEXT_TERTIARY     0x6390u /* #667080 captions/hints        */
#define MICHI_UI_ACCENT            0xFAF1u /* #FF5C8A Michi pink            */
#define MICHI_UI_ACCENT_SOFT       0xE28Fu /* #E84F7B pressed/soft pink     */
#define MICHI_UI_SUCCESS           0x5EB3u /* #5AD6A0 ok/playing            */
#define MICHI_UI_WARNING           0xEDABu /* #F2B85B warn/degraded         */
#define MICHI_UI_ERROR             0xFB0Cu /* #FF6262 fatal/rejected        */
#define MICHI_UI_INFO              0x6D5Fu /* #68A8FF neutral info          */
#define MICHI_UI_MUTED             0x52ACu /* #505765 disabled/inactive     */

/**
 * @brief Convert an RGB888 triplet to RGB565 with rounding.
 *
 * channel5 = (c * 31 + 127) / 255; channel6 = (c * 63 + 127) / 255.
 */
static inline uint16_t michi_ui_rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)((((uint16_t)r * 31u + 127u) / 255u) << 11) |
           (uint16_t)((((uint16_t)g * 63u + 127u) / 255u) << 5) |
           (uint16_t)(((uint16_t)b * 31u + 127u) / 255u);
}

/*!< Palette entry: name/value pair (tests iterate this table). */
typedef struct michi_ui_theme_entry {
    const char *name;
    uint16_t value;
} michi_ui_theme_entry_t;

#define MICHI_UI_THEME_ENTRY_COUNT 13

/*!< The full semantic palette, stored const in flash. */
extern const michi_ui_theme_entry_t michi_ui_theme_palette[MICHI_UI_THEME_ENTRY_COUNT];

#ifdef __cplusplus
}
#endif

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

#define MICHI_UI_BG                MICHI_UI_RGB565(0x08, 0x0A, 0x0F) /* near-black canvas */
#define MICHI_UI_SURFACE           MICHI_UI_RGB565(0x10, 0x14, 0x1A) /* cards/panels     */
#define MICHI_UI_SURFACE_ELEVATED  MICHI_UI_RGB565(0x15, 0x1A, 0x24) /* raised surfaces  */
#define MICHI_UI_TEXT_PRIMARY      MICHI_UI_RGB565(0xF5, 0xF6, 0xF8) /* headings/values  */
#define MICHI_UI_TEXT_SECONDARY    MICHI_UI_RGB565(0x9A, 0xA3, 0xB3) /* body text        */
/* Tertiary was #667080 (3.95:1 on the canvas, below the WCAG 4.5 target for
 * informative text). #76808F keeps the same hue family and reaches 4.95:1 on
 * the canvas and 4.62:1 on a surface. */
#define MICHI_UI_TEXT_TERTIARY     MICHI_UI_RGB565(0x76, 0x80, 0x8F) /* captions/hints   */
#define MICHI_UI_ACCENT            MICHI_UI_RGB565(0xFF, 0x5C, 0x8A) /* Michi pink       */
#define MICHI_UI_ACCENT_SOFT       MICHI_UI_RGB565(0xE8, 0x4F, 0x7B) /* pressed/soft     */
#define MICHI_UI_SUCCESS           MICHI_UI_RGB565(0x5A, 0xD6, 0xA0) /* ok/playing       */
#define MICHI_UI_WARNING           MICHI_UI_RGB565(0xF2, 0xB8, 0x5B) /* warn/degraded    */
#define MICHI_UI_ERROR             MICHI_UI_RGB565(0xFF, 0x62, 0x62) /* fatal/rejected   */
#define MICHI_UI_INFO              MICHI_UI_RGB565(0x68, 0xA8, 0xFF) /* neutral info     */
#define MICHI_UI_MUTED             MICHI_UI_RGB565(0x50, 0x57, 0x65) /* disabled (exempt from 4.5) */

#define MICHI_UI_PANEL_W           320
#define MICHI_UI_PANEL_H           240

/*!< Safe area margins */
#define MICHI_UI_SAFE_LEFT         16
#define MICHI_UI_SAFE_RIGHT        16
#define MICHI_UI_SAFE_TOP          12
#define MICHI_UI_SAFE_BOTTOM       12

/*!< Vertical grid constants (Landscape) */
#define MICHI_UI_HEADER_Y          14
#define MICHI_UI_DIVIDER_Y         42
#define MICHI_UI_CONTENT_TOP_Y     62
#define MICHI_UI_FOOTER_Y          211

/**
 * @brief Convert an RGB888 triplet to RGB565 with rounding.
 *
 * channel5 = (c * 31 + 127) / 255; channel6 = (c * 63 + 127) / 255.
 */
#define MICHI_UI_RGB565(r, g, b)                                             \
    ((uint16_t)(((((uint16_t)(r) * 31u + 127u) / 255u) << 11) |              \
                ((((uint16_t)(g) * 63u + 127u) / 255u) << 5) |               \
                ((((uint16_t)(b) * 31u + 127u) / 255u))))

static inline uint16_t michi_ui_rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return MICHI_UI_RGB565(r, g, b);
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

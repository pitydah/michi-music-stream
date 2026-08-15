#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Monochrome bitmap icon API (UI redesign, phase UI-01).
 *
 * Icons are procedural stroke art (committed generator script in tools/),
 * 1 bpp row-major MSB-first, stored const in flash (assets/icons/).
 * Available sizes: 12/20/32 for every icon; the cat also at 24 and 48.
 * Drawing an unavailable combination is a silent no-op.
 */

/*!< Icon identifiers. */
typedef enum michi_ui_icon {
    MICHI_UI_ICON_CAT = 0, /*!< Michi cat face */
    MICHI_UI_ICON_WIFI,    /*!< Wi-Fi arcs */
    MICHI_UI_ICON_SERVER,  /*!< Server rack */
    MICHI_UI_ICON_SPEAKER, /*!< Speaker */
    MICHI_UI_ICON_PLAY,    /*!< Play triangle */
    MICHI_UI_ICON_PAUSE,   /*!< Pause bars */
    MICHI_UI_ICON_PAIR,    /*!< Pairing circles */
    MICHI_UI_ICON_BUTTON,  /*!< Physical button */
    MICHI_UI_ICON_WARNING, /*!< Warning triangle */
    MICHI_UI_ICON_ERROR,   /*!< Error circle */
    MICHI_UI_ICON_UPDATE,  /*!< Download/update arrow */
    MICHI_UI_ICON_WAVE,    /*!< Audio wave */
    MICHI_UI_ICON_COUNT,
} michi_ui_icon_t;

/*!< Icon sizes in pixels (indexed 0..4, not raw px). */
typedef enum michi_ui_icon_size {
    MICHI_UI_ICON_SIZE_12 = 0,
    MICHI_UI_ICON_SIZE_20,
    MICHI_UI_ICON_SIZE_24,
    MICHI_UI_ICON_SIZE_32,
    MICHI_UI_ICON_SIZE_48,
    MICHI_UI_ICON_SIZE_COUNT,
} michi_ui_icon_size_t;

/**
 * @brief Draw an icon with its top-left at ABSOLUTE (x, y), clipped to the
 *        band [0, fb_h) in local Y and [0, fb_w) in X. No-op when the
 *        icon/size combination is not generated.
 */
void ui_draw_icon(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                  uint16_t y_origin, int x, int y, michi_ui_icon_t icon,
                  michi_ui_icon_size_t size, uint16_t color);

#ifdef __cplusplus
}
#endif

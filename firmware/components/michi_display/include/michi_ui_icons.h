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

#define MICHI_ICON_CAT     MICHI_UI_ICON_CAT
#define MICHI_ICON_WIFI    MICHI_UI_ICON_WIFI
#define MICHI_ICON_SERVER  MICHI_UI_ICON_SERVER
#define MICHI_ICON_SPEAKER MICHI_UI_ICON_SPEAKER
#define MICHI_ICON_PLAY    MICHI_UI_ICON_PLAY
#define MICHI_ICON_PAUSE   MICHI_UI_ICON_PAUSE
#define MICHI_ICON_PAIR    MICHI_UI_ICON_PAIR
#define MICHI_ICON_BUTTON  MICHI_UI_ICON_BUTTON
#define MICHI_ICON_WARNING MICHI_UI_ICON_WARNING
#define MICHI_ICON_ERROR   MICHI_UI_ICON_ERROR
#define MICHI_ICON_UPDATE  MICHI_UI_ICON_UPDATE
#define MICHI_ICON_WAVE    MICHI_UI_ICON_WAVE

/**
 * @brief Draw an icon with its top-left at ABSOLUTE (x, y), clipped to the
 *        band [0, fb_h) in local Y and [0, fb_w) in X. No-op when the
 *        icon/size combination is not generated.
 */
void ui_draw_icon(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                  uint16_t y_origin, int x, int y, michi_ui_icon_t icon,
                  michi_ui_icon_size_t size, uint16_t color);

/**
 * @brief Helper that maps pixel size to the nearest supported enum size.
 */
static inline void michi_ui_draw_icon(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                     uint16_t y_origin, int x, int y,
                                     michi_ui_icon_t icon, int px_size, uint16_t color)
{
    michi_ui_icon_size_t sz = MICHI_UI_ICON_SIZE_20;
    if (px_size <= 14) sz = MICHI_UI_ICON_SIZE_12;
    else if (px_size <= 22) sz = MICHI_UI_ICON_SIZE_20;
    else if (px_size <= 28) sz = MICHI_UI_ICON_SIZE_24;
    else if (px_size <= 40) sz = MICHI_UI_ICON_SIZE_32;
    else sz = MICHI_UI_ICON_SIZE_48;
    ui_draw_icon(fb, fb_w, fb_h, y_origin, x, y, icon, sz, color);
}

#ifdef __cplusplus
}
#endif

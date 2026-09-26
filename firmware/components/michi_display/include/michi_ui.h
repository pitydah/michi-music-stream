#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Michi UI design system - shared primitives (UI redesign, phase UI-01).
 *
 * This header is the public entry point of the UI infrastructure layer:
 * geometry, the semantic color palette, the bitmap typography system, the
 * text API and the icon API. The existing michi_display screens are NOT
 * migrated in this phase - later phases replace their ad-hoc drawing with
 * these primitives.
 *
 * Band contract (MS-11, inherited from michi_board): every draw call takes
 * the band framebuffer (fb_w x fb_h) plus y_origin, the absolute top row of
 * the band. Layout coordinates stay ABSOLUTE (panel rows); the APIs convert
 * to the local band row internally and clip per pixel, so a glyph/icon
 * straddling a band boundary is split correctly across two flushes.
 */

/*!< Rectangle in absolute panel coordinates (dirty-region groundwork). */
typedef struct michi_ui_rect {
    int x;
    int y;
    int w;
    int h;
} michi_ui_rect_t;

/*!< Horizontal text alignment inside a rectangle. */
typedef enum michi_ui_align {
    MICHI_UI_ALIGN_LEFT = 0,
    MICHI_UI_ALIGN_CENTER,
    MICHI_UI_ALIGN_RIGHT,
} michi_ui_align_t;

#include "michi_ui_theme.h"
#include "michi_ui_strings.h"
#include "michi_ui_fonts.h"
#include "michi_ui_text.h"
#include "michi_ui_icons.h"
#include "michi_ui_components.h"
#include "michi_ui_screens.h"

#ifdef __cplusplus
}
#endif

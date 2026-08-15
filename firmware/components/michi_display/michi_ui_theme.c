#include "michi_ui_theme.h"

/*
 * Semantic palette registry. Every entry references the canonical macro from
 * michi_ui_theme.h; the table exists so host tests can verify the RGB888 ->
 * RGB565 conversion and future tools can dump the palette without touching
 * the renderer.
 */
const michi_ui_theme_entry_t michi_ui_theme_palette[MICHI_UI_THEME_ENTRY_COUNT] = {
    { "bg", MICHI_UI_BG },
    { "surface", MICHI_UI_SURFACE },
    { "surface_elevated", MICHI_UI_SURFACE_ELEVATED },
    { "text_primary", MICHI_UI_TEXT_PRIMARY },
    { "text_secondary", MICHI_UI_TEXT_SECONDARY },
    { "text_tertiary", MICHI_UI_TEXT_TERTIARY },
    { "accent", MICHI_UI_ACCENT },
    { "accent_soft", MICHI_UI_ACCENT_SOFT },
    { "success", MICHI_UI_SUCCESS },
    { "warning", MICHI_UI_WARNING },
    { "error", MICHI_UI_ERROR },
    { "info", MICHI_UI_INFO },
    { "muted", MICHI_UI_MUTED },
};

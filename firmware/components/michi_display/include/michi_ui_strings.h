#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Centralized UI string constants for Michi Music Stream.
 *
 * All text displayed on the screen is defined here to ensure consistent
 * terminology, maintain a premium tone, and facilitate future internationalization.
 */

/* Brand & Header */
#define MICHI_UI_STR_BRAND             "Michi"
#define MICHI_UI_STR_BRAND_LOWER       "michi"
#define MICHI_UI_STR_DIAGNOSTICS_TITLE "Michi Diagnostics"

/* Boot */
#define MICHI_UI_STR_STARTING          "iniciando"

/* Ready / Idle */
#define MICHI_UI_STR_READY             "Listo"
#define MICHI_UI_STR_WAITING_PLAYBACK  "Esperando reproducción"

/* Unprovisioned */
#define MICHI_UI_STR_SETUP_TITLE       "Configurar Michi"
#define MICHI_UI_STR_SETUP_HINT        "Mantén presionado el botón para comenzar"

/* Provisioning / Wi-Fi */
#define MICHI_UI_STR_PROV_TITLE        "Configurando red"
#define MICHI_UI_STR_PROV_HINT         "Conecta Michi a tu red desde el dispositivo de configuración"
#define MICHI_UI_STR_CONNECTING_TITLE  "Conectando"
#define MICHI_UI_STR_CONNECTING_WIFI   "A la red Wi-Fi"

/* Pairing */
#define MICHI_UI_STR_PAIRING_TITLE     "Vincular"
#define MICHI_UI_STR_PAIRING_LINKING   "Vinculando"
#define MICHI_UI_STR_PAIRING_WAITING   "Esperando al servidor..."
#define MICHI_UI_STR_PAIRING_PIN_HINT  "Introduce este código en Michi Micro Server"

/* Playback & Session */
#define MICHI_UI_STR_SESSION_PREP      "Preparando reproducción"
#define MICHI_UI_STR_AUDIO_PREP        "Preparando audio"
#define MICHI_UI_STR_PLAYING           "Reproduciendo"
#define MICHI_UI_STR_PAUSED            "Pausa"
#define MICHI_UI_STR_DEFAULT_SERVER    "Michi Micro Server"

/* Update */
#define MICHI_UI_STR_UPDATING_TITLE    "Actualizando"
#define MICHI_UI_STR_UPDATING_HINT     "No desconectes Michi"

/* Errors */
#define MICHI_UI_STR_RECOVERING_TITLE      "Reconectando"
#define MICHI_UI_STR_RECOVERING_HINT       "Intentando recuperar la conexión"
#define MICHI_UI_STR_RECOVERING_AUDIO      "Recuperando audio"
#define MICHI_UI_STR_RECOVERING_AUDIO_HINT "Intentando recuperar la reproducción"
#define MICHI_UI_STR_FATAL_TITLE           "Algo falló"
#define MICHI_UI_STR_FATAL_HINT            "Reinicia Michi Stream"
#define MICHI_UI_STR_ERROR_CODE_PREFIX     "Código: "

/* Volume overlay & Diagnostics */
#define MICHI_UI_STR_VOLUME_TITLE          "Volumen"
#define MICHI_UI_STR_DAC_NONE              "No detectado"
#define MICHI_UI_STR_DAC_UNKNOWN           "Desconocido"

#ifdef __cplusplus
}
#endif

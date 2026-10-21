# Michi Music Stream — Landscape UI Architecture (320 × 240)

Este documento describe la interfaz visual landscape de **Michi Music Stream** implementada sobre la placa **Waveshare ESP32-S3-LCD-2** (controlador ST7789, panel IPS 320×240 RGB565).

---

> **Estado de la galería (UI-01, 2026-10-18):** las capturas PNG enlazadas aquí son vistas previas **históricas** y todavía no han sido regeneradas a partir del diseño Quiet Hi-Fi aplicado a `michi_ui_screens.c`. En particular, `ui-02-unprovisioned.png`, `ui-05-pairing-waiting.png` y `ui-06-pairing-pin.png` corresponden al flujo antiguo de pairing por botón/PIN, retirado del firmware operativo. No presentar esas imágenes como UI actual ni como evidencia física. Consultar `UI01_QUIET_HIFI_DESIGN.md` para el nuevo sistema de jerarquía visual y el procedimiento de actualización de previews.

## 1. Principios de Diseño

* **Estética Quiet Hi-Fi (UI-01)**: superficies sólidas RGB565, bandas de información y cabecera/pie consistentes, sin vidrio translúcido ni blur.
* **Estética Hi-Fi Premium**: Fondo profundo `#080A0F`, superficies `#10141A` y `#151A24`, acento Michi `#FF5C8A`, tipografía abierta Noto Sans (SIL Open Font License).
* **Device Truth**: El header, footer y pantalla de diagnóstico consumen el estado real de los subsistemas (`michi_wifi`, `michi_session`, `michi_dac`, `michi_volume`, `michi_ota`). No se utilizan valores simulados en producción.
* **Memoria DMA Eficiente (MS-11)**: Renderizado por bandas secuenciales (6 bandas de 320×40 px, 25.6 KB DMA RAM por banda) sin requerir framebuffer completo en memoria interna.

---

## 2. Taxonomía de Errores y Clasificación

Los errores se clasifican de forma unificada mediante `michi_ui_classify_error()`:

| Clase | Códigos de Origen | Título en Pantalla | Subtítulo / Acción | Código Visible |
|---|---|---|---|---|
| **Network** | `0x3000..0x5FFF` | Reconectando | Recuperando la conexión | `Código E101` |
| **Audio** | `0x103`, `0x107`, `0x7000..0x7FFF` | Recuperando audio | Restableciendo reproducción | `Código E102` |
| **Storage** | `0x2000..0x2FFF` | Error del sistema | Reinicia el dispositivo | `Código E103` |
| **Memory** | `0x101` (`ESP_ERR_NO_MEM`) | Error del sistema | Reinicia el dispositivo | `Código E104` |
| **Update** | `0x6000..0x6FFF` | Error del sistema | Reinicia el dispositivo | `Código E105` |
| **Unknown** | Otros | Error del sistema | Reinicia el dispositivo | `Código E199` |

---

## 3. Estado del DAC en Diagnóstico

El subsistema de diagnóstico consulta las capacidades reales mediante `michi_dac_get_caps()`:

* **Presente**: Muestra el modelo detectado (e.g. `PCM5122`) en verde (`#5AD6A0`).
* **No detectado**: Muestra `No detectado` en ámbar (`#F2B85B`).
* **Desconocido**: Muestra `Desconocido` en color tenue (`#505765`).
* **Error**: Muestra `Error` en rojo (`#FF6262`).

---

## 4. Galería de Estados (Previews 320 × 240)

| Escenario | Vista Previa |
|---|---|
| **ui-01: Boot** | ![Boot](previews/ui-01-boot.png) |
| **ui-02: Unprovisioned** | ![Unprovisioned](previews/ui-02-unprovisioned.png) |
| **ui-03: Ready** | ![Ready](previews/ui-03-ready.png) |
| **ui-04: Connecting** | ![Connecting](previews/ui-04-connecting.png) |
| **ui-05: Pairing Waiting** | ![Pairing Waiting](previews/ui-05-pairing-waiting.png) |
| **ui-06: Pairing PIN** | ![Pairing PIN](previews/ui-06-pairing-pin.png) |
| **ui-07: Session Pending** | ![Session Pending](previews/ui-07-session-pending.png) |
| **ui-08: Buffering (Meta)** | ![Buffering Meta](previews/ui-08-buffering-meta.png) |
| **ui-09: Buffering (No Meta)** | ![Buffering No Meta](previews/ui-09-buffering-nometa.png) |
| **ui-10: Playing** | ![Playing](previews/ui-10-playing-meta.png) |
| **ui-11: Playing (UTF-8)** | ![Playing UTF-8](previews/ui-11-playing-spanish-utf8.png) |
| **ui-13: Paused** | ![Paused](previews/ui-13-paused-meta.png) |
| **ui-15: Updating (Progress)** | ![Updating Progress](previews/ui-15-updating-progress.png) |
| **ui-15b: Updating (Indeterminate)** | ![Updating Indeterminate](previews/ui-15b-updating-unknown.png) |
| **ui-16: Error (Audio)** | ![Error Audio](previews/ui-16-recoverable-error-audio.png) |
| **ui-17: Error (Network)** | ![Error Network](previews/ui-17-recoverable-error-network.png) |
| **ui-18: Fatal Error** | ![Fatal Error](previews/ui-18-fatal-error.png) |
| **ui-19: Diagnostics (Connected)** | ![Diagnostics](previews/ui-19-diagnostics-connected.png) |
| **ui-19b: Diagnostics (DAC Absent)** | ![Diagnostics DAC Absent](previews/ui-19b-diagnostics-dac-absent.png) |
| **ui-20: Volume Overlay** | ![Volume Overlay](previews/ui-20-volume-overlay.png) |


## UI-02 — Signature Hi-Fi (actualización 2026-10-18)

La especificación vigente de diseño y las pruebas físicas se encuentran en
[`UI02_SIGNATURE_HIFI_DESIGN.md`](UI02_SIGNATURE_HIFI_DESIGN.md).
La galería histórica (`previews/`) **no** es la UI actual. Capturas nuevas
del renderizador C en `previews-ui02/` (RGB565 de 320×240, no fotos del hardware).
SVG oficial en `assets/brand/`, raster 40/72 px en flash.
El modo de diagnóstico físico puede habilitarse solamente en builds de
laboratorio con `CONFIG_MICHI_DISPLAY_DIAGNOSTICS_ON_BOOT=y`; disabled por
defecto en producción. OTA y fatal deben prevalecer siempre.

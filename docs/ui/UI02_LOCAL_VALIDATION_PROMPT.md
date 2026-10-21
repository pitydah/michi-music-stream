# OpenCode — Validación local Signature Hi-Fi UI-02

Modelo sugerido: DeepSeek Flash. Ejecución sobre copia local descargada de Drive; no realizar refactor masivo de audio ni protocolos.

## Objetivo

Cerrar coherencia UI y probar sin hardware primero, después ensayos con ESP32-S3 y PCM5122. Cambios en Drive del 18 oct 2026 se editaron sin compilar el firmware ESP-IDF ni ejecutar la suite completa. No informar PASS sin evidencia real.

## Reconciliación obligatoria

1. Haz copia git o crea una rama de seguridad local. La copia de Drive contiene fuentes que pueden ser más nuevos que su `.git` histórico: inspecciona `git status`, `git diff` y conserva `HEAD` como referencia, sin usar un reset destructivo.
2. Comprueba existencia del logo oficial `firmware/components/michi_display/assets/brand/michi_stream_official.svg`, bitmap `assets/icons/michi_stream_brand_data.h` y generador `tools/gen_michi_brand.py`.
3. Comprueba la concordancia entre `CONFIG_MICHI_DISPLAY_RASTER_2BIT=y` y el header de Noto Sans 2 bit.
4. Aplica cuidadosamente `docs/ui/UI02_PENDING_LOCAL_CHANGES.patch` únicamente si las diferencias aún faltan. Usa `git apply --check` antes de aplicarlo. No reescribas archivos que ya coincidan.
5. Revisa que la implementación de `michi_ui_draw_brand()` compile, que dibuje por bandas sin heap y que NO procese SVG en runtime.

## Validación host y firmware

- `python3 -m pytest tests/host/test_ui02_brand_assets.py -q`
- `python3 firmware/components/michi_display/tools/gen_michi_brand.py`
- `cd firmware/components/michi_display && python3 tools/gen_michi_ui_assets.py --raster 2bit --strict`
- `make -C tests/host clean test_michi_ui`
- `./tests/host/test_michi_ui`
- Ejecución específica de pruebas DMA según la meta disponible en `tests/host/Makefile`.
- Regenerar todas las capturas host desde `render_ui_previews.c`, comparar con `docs/ui/previews-ui02/` y realizar inspección visual de contrastes, recortes y legibilidad sobre 320×240.
- `idf.py set-target esp32s3 && idf.py build` con ESP-IDF 5.3 y ambiente correcto.
- Si fallan snapshots/fixture de pairing histórico, actualizar mocks obsoletos sin restaurar pairing físico en producción. No falsear la transición de estados.

## Matrix de pruebas físicas en placa

Activar `CONFIG_MICHI_DISPLAY_DIAGNOSTICS_ON_BOOT=y` en **un build de laboratorio**, manteniendo `n` para distribución final. Registrar fotos y logs: boot, Wi-Fi, IP, SNTP, mDNS, TX firmados, aceptación de firmas por Micro Server/WebUI, autenticación Michi Home, sesión RTP, DAC real y audio PCM, reconexión Wi-Fi, OTA. Repetir con SNTP caído, mDNS fallido, DAC ausente, Wi-Fi sin IP y PSRAM no medida; no reemplazar estados desconocidos por valores predeterminados.

Aceptar cierre solo si todos los checks alcanzados tienen logs verificables. Hardware/Audio=UNVERIFIED hasta pruebas en placa. Entregar resumen con SHA, archivos corregidos, tests reales, límites y capturas de pantalla por estado.

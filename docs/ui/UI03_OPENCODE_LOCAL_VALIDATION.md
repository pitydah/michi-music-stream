# Prompt OpenCode — validación local de Michi Music Stream UI-03

Trabaja en `michi-music-stream`, tras descargar y reemplazar los fuentes de Drive. No rediseñes ni modifiques la ruta de audio salvo para corregir una incompatibilidad probada.

1. Audita el diff real de `firmware/components/michi_display/michi_ui_screens.c`, `michi_display.c`, `include/michi_ui_screens.h` y `tests/host/render_ui_previews.c` y el nuevo test `test_ui03_wave_previews.py`. Confirma que no hay pérdidas respecto a UI-02.
2. Compila host bajo GCC/Clang con warnings estrictos; ejecuta todas las pruebas UI y DMA existentes. Genera los previews y ejecuta `python3 tests/host/test_ui03_wave_previews.py <directorio_ppm>`.
3. Compila ESP-IDF 5.3 (`idf.py set-target esp32s3 && idf.py build`). Si hay errores corrige integración en fuentes; conserva identidad, descubrimiento, audio RTP/I2S y seguridad.
4. En placa mide tiempo por full frame, FPS real, CPU de display, memoria y posibles underruns I2S durante 10 min de PLAYING y transiciones BUFFERING→PLAYING→PAUSED. Asegúrate de que el refresco ocurre solo cuando hay actividad y sin riesgo de bloquear audio.
5. Verifica visualmente sin recortes wave, título/autor en UTF-8, fuente inferior, pie volumen/formato, overlays, fallo fatal, OTA y diagnóstico. Registra capturas reales.
6. Entrega reporte KILLCRITIC con fallos, correcciones, SHA de HEAD, comandos/logs y qué se ha probado realmente. No declares DEVICE_E2E_PASS sin hardware.

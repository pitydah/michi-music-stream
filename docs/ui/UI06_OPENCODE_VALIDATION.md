# Prompt para OpenCode — validación local UI-06 Michi Music Stream

**Objetivo:** validar, NO rediseñar, el código UI-06 de la copia de Drive reemplazada en disco local. No modificar RTP/I2S, identidad, pairing ni Wi-Fi fuera de lo necesario para diagnosticar errores. No asumir pruebas físicas realizadas.

1. Verificar los diffs de `michi_ui_screens.c`, `michi_ui_icons.c`, `michi_ui_components.c` e `include/michi_ui_icons.h` y confirmar que `MICHI_UI_ICON_ROOM` está anexado al final del enum, antes de COUNT.
2. Ejecutar `python firmware/components/michi_display/tools/gen_precision_icons.py --output firmware/components/michi_display/assets/icons/michi_precision_icons_data.h` y comprobar que no hay cambios de contenido ni de SHA256; si Pillow cambió resultados, reportar, no reescribir silenciosamente el archivo de producción.
3. Compilar ESP-IDF desde `firmware` con su versión fijada de toolchain y PSRAM/configuración real. Vigilar warnings, flash y stack; no alterar `michi_display` priority ni el loop I2S.
4. Compilar el renderizador host `tests/host/render_ui_previews.c`, regenerar las capturas PPM en una carpeta temporal y ejecutar `python tests/host/test_ui06_visual_contracts.py` tras disponer `build/previews` localmente; si falla, adjuntar diff mínimo y explicación.
5. Ejecutar suite de host UI, DMA bands, fuentes UTF-8, truncamiento, iconos, todos los estados (boot, ready, buffering, playing, paused, OTA, error fatal, diag) y sanitizers compatibles, con énfasis en memoria/índices.
6. En hardware, fotografiar Wi-Fi conectado con RSSI bueno/débil/desconectado, receptor conectado/desconectado, source "Living Room", título largo español y pantalla de diagnóstico. Evaluar 12/16px a 40–80cm y contraste con iluminación real.
7. Con audio real, comparar la reproducción I2S/PCM5122 con UI animada y desactivada, midiendo carga de CPU, tiempo SPI, tasa real FPS, underruns, jitter buffer y desconexiones de red. No fingir resultados.
8. Cierre obligatorio: tabla PASS/FAIL/NOT RUN para cada prueba y hardware, logs relevantes, cambios correctivos imprescindibles y regresiones. No declarar 100% ni 'ready to ship' por solo ver PNGs.

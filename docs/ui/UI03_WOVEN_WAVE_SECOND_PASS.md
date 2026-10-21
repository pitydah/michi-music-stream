# Michi Music Stream — UI-03 / Woven Wave (segunda pasada)

**Objetivo:** quitar las barras coloreadas que degradaban las pantallas de reproducción, bajar la fuente (sala/servidor) y sustituir la decoración por una animación de ondas entrelazadas inspirada en la referencia aportada por el usuario. Modificar las pantallas reales del firmware, no solo mockups.

## Implementación

- `michi_ui_screens.c`: se retiran las barras verticales de 3×69 px en `PLAYING` y `PAUSED`; en reproducción el separador superior se dibuja neutro. Se elimina la tarjeta de fuente a y=175, que interrumpía la composición.
- **Nueva franja Woven Wave**: tres trazos azul-gris muy finos, amplitud atenuada en ambos extremos, combinación de dos senoidales mediante LUT de 64 enteros. Solo aritmética entera y llamadas recortadas por banda RGB565 (sin FFT, sin DSP, sin heap, sin GPU ni carga de PNG/SVG dinámica).
- `PLAYING`: movimiento continuo, discretamente orgánico; `BUFFERING`: movimiento con amplitud algo más pequeña; `PAUSED`: ondas prácticamente planas, sin refrescos periódicos.
- La fuente aparece abajo, como texto e icono pequeño en y=197, sin gran tarjeta de fondo. Se preserva el pie con volumen y formato.
- `michi_ui_screens.h`: `waveform_phase` pasa a formar parte del snapshot de pantalla. Cada conjunto de bandas de 320×40 consume la **misma fase**, evitando cortes horizontales o estados inconsistentes entre bandas.
- `michi_display.c`: la tarea de display hace un render cada 160 ms **solo** durante reproducción/buffering sin overlays. Nunca introduce una tarea adicional de alta prioridad, ni toca el audio o el estado de sesión. En idle, pausa, errores, diagnósticos o OTA vuelve a esperar eventos (sin animación). La fase la determina el reloj monotónico; no se incrementa al dibujar cada banda.
- `render_ui_previews.c`: 12 fases reproducibles en `ui-21-wave-NN.ppm` más escenarios habituales.
- `test_ui03_wave_previews.py`: checks de región animada, eliminación de acento y ausencia de dibujo fuera del área de la onda.

## LIMITACIÓN de telemetría

La onda es **una animación de actividad de reproducción**, NO una forma de onda procedente de muestras PCM, NO un visualizador FFT y NO una barra real de progreso. No representa amplitud, calidad, jitter ni elapsed. Si se negocia más adelante un campo `elapsed`/`duration`, debe incorporarse como componente distinto con la fuente de verdad apropiada.

## Validaciones realizadas

- Compilación host del motor de pantallas y render de 38 escenarios PPM con `-Wall -Wextra -Werror`: satisfactoria.
- Test de diferencias de 12 fases: satisfactorio; los cambios están acotados a la banda de movimiento y las líneas de acento fueron eliminadas de reproducción/pausa.
- Galería estática y GIF generados usando el renderizador real de pantallas con 6 bandas.

**NO certificados:** compilación global ESP-IDF, consumo real de CPU/SPI, impacto en I2S, FPS físico, calidad óptica del panel y estabilidad prolongada. Evaluar con dispositivo real.

## Validación local recomendada (OpenCode)

1. Después de descargar la carpeta Drive, ejecutar `git diff --check` e inspeccionar cambios en los cuatro archivos C/H del sprint.
2. Construir `firmware/` con ESP-IDF 5.3 para `esp32s3`. Corregir cualquier error de integración sin tocar la ruta de audio.
3. Reconstruir el test de previews y ejecutar `python3 tests/host/test_ui03_wave_previews.py tests/host/previews` tras generar PPM.
4. En ESP32 real, reproducir continuamente 10 minutos y medir refresco SPI, uso CPU de `michi_display`, I2S underruns, jitter y estabilidad de sesiones. Confirmar que **el display no dificulta el audio**. Si hay anomalías, reducir temporalmente el FPS o desactivar la animación hasta corregirla.
5. Verificar vídeo/fotografías de `PLAYING`, `BUFFERING`, `PAUSED`, título largo en español, overlay volumen, OTA, fatal y diagnósticos. No certificar el hardware con gifs host.

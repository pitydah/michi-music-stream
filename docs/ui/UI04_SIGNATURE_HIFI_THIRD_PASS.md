# Michi Music Stream — UI-04: Tercer pulido (Signature Hi-Fi)

## Alcance y origen

Iteración sobre la implementación **UI-03 Woven Wave** existente. No es una sustitución del diseño del receptor. Mantiene el logotipo oficial, la pantalla landscape 320×240 ST7789 RGB565, los iconos existentes y el renderizador de seis bandas 320×40. No cambia la pila RTP, autenticación o DAC.

## Cambios hechos en código

1. **Woven Wave antialias:** interpolación de seno LUT en Q2; dos píxeles de cobertura vertical mediante mezcla RGB565 entera de 4 bits. Permanece la superposición discreta de tres ondas azul-gris, sin barras de color, gradientes costosos ni DSP. El movimiento es decorativo: **NO es una forma de onda PCM ni un slider/progreso real**.
2. **Ahorro de CPU:** el dibujo de la onda retorna inmediatamente en las cinco bandas DMA fuera de y=163..190. No asigna heap, no usa trigonometría en tiempo de ejecución ni lee buffers de audio.
3. **Jerarquía tipográfica:** coordenadas distintas para títulos musicales de una o dos líneas; artista con línea base fija según las líneas del título; se reserva una franja independiente de la onda y de la fuente. Mantiene UTF-8 y truncado seguro.
4. **Fuentes/zonas:** el identificador del origen permanece compacto y más abajo (sin tarjeta de fondo). Footer de volumen y formato sin cambios de función. Línea de acento superior y barras verticales siguen eliminadas.
5. **Configuración de laboratorio:** `MICHI_DISPLAY_WAVE_ANIMATION` (predeterminado `y`, desactivable en `menuconfig`) y `MICHI_DISPLAY_WAVE_PERIOD_MS` (120..500 ms, valor predeterminado 160). Desactivar elimina refrescos periódicos; mantiene la pantalla y dibuja una onda estática con fase cero.
6. **Previews:** extensión del generador de host con escenarios para UTF-8 y pausa con fase no cero. Archivos PPM/GIF comparativos en `docs/ui/previews-ui04/`.

## Límites y cuidado con el audio

- No se ha ejecutado `idf.py build`, ni se ha instalado o medido en ESP32-S3. La compilación del renderizador de previews en host sirve **solo** para inspección visual, no certifica rendimiento del LCD.
- El refresco continúa realizando el render/flush de la pantalla completa a través del mecanismo de banda existente; el ahorro CPU afecta al cálculo de la onda, **no** al tráfico SPI. La opción de configuración permite apagarlo si produce interferencias I2S.
- El coste de mezcla por píxel se ha limitado a la región pequeña de onda. No inferir FPS físico, ausencia de underruns, sincronización de tiempo o calidad de audio por una captura.
- No se introduce `elapsed`/`duration` artificial: la animación no representa progreso de canción.

## Archivos modificados

- `firmware/components/michi_display/michi_ui_screens.c`
- `firmware/components/michi_display/michi_display.c`
- `firmware/components/michi_display/Kconfig`
- `firmware/sdkconfig.defaults`
- `tests/host/render_ui_previews.c`
- `tests/host/test_ui04_visual_invariants.py` (nuevo)

## Entrega para validación local

Consultar `UI04_OPENCODE_VALIDATION.md`. Priorizar pruebas físicas de los estados reproduciendo/buffering/pausa y comparar tasa de underruns I2S con animación activada y desactivada. Si se detecta cualquier regresión de audio, desactivar de inmediato la animación y dejar la ruta de audio sin cambios.

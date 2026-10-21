# Michi Music Stream — UI-06 Signature Hi-Fi / quinta pasada KILLCRITIC

Fecha: 2026-10-18. Dispositivo destino: Waveshare ESP32-S3-LCD-2, 320x240 RGB565, bandas DMA de 320x40.

## Autoridad de la versión

Este sprint parte de los archivos originales UI-04 que se encontraban en la copia **michi-music-stream** de Google Drive. El concepto de la cuarta pasada local (UI-05) —Wi-Fi refinado, ubicación y cuatro ondas— no estaba consolidado en Drive. UI-06 implementa ese objetivo mediante una solución reproducible y auditada; no depende de fuentes UI-05 desaparecidos.

## Auditoría KILLCRITIC

### P0 visual / identidad

- Wi-Fi generado con arcos antiguos: apariencia de nube a 16 px, mala lectura del estado.
- `SERVER` anterior: rack tosco, desalineado con la escala de la interfaz de un receptor moderno.
- Etiqueta de origen `Living Room / Michi Studio`: icono de servidor incorrecto semánticamente para sala / zona.
- Icono de volumen: ancho de trazo no consistente con los otros símbolos de la barra.
- Documentación del cuarto pulido no equivalía a una subida completa del código a Drive.

### P1 interacción / diagnóstico

- Encabezados de las dos columnas de diagnóstico a 8 px demasiado pequeños para inspección sobre LCD IPS real.
- La onda había ganado suavidad en UI-04, pero seguía teniendo tres capas de intensidad similar; se beneficia de separación tonal y una cuarta capa discreta.
- Wi-Fi débil no se distinguía visualmente del Wi-Fi conectado con señal aceptable.
- No hay datos PCM de volumen dinámico; la onda es **decorativa**, nunca un medidor de audio ni un indicador de progreso.

## Implementación UI-06

1. Nuevo `tools/gen_precision_icons.py`: define Wi-Fi, receptor, sala y volumen con geometrías simplificadas y antialiasing LANCZOS de 8x; cuantiza determinísticamente cobertura RGBA a 2 bpp.
2. Nuevo `assets/icons/michi_precision_icons_data.h`: 16 recursos en flash (4 símbolos × 4 tamaños: 12, 16, 20, 32). Sin SVG ni Pillow en runtime, sin memoria dinámica.
3. `michi_ui_icons.c`: despachador de los nuevos pictogramas con blend de cobertura RGB565 (cada lectura/escritura queda acotada por banda y por pantalla). Los iconos legados mantienen el camino 1 bpp, con compatibilidad retroactiva.
4. `michi_ui_icons.h`: añade `MICHI_UI_ICON_ROOM` al final del enum para conservar los identificadores históricos.
5. `michi_ui_screens.c`: reemplaza el rack anterior junto a la ubicación por un símbolo de sala; onda de cuatro capas tenues y distinta cinemática para PLAYING, BUFFERING y PAUSED. Los encabezados de RED/DISCOVERY y AUDIO/HARDWARE crecen a `SM`; etiquetas de diagnóstico en color secundario.
6. `michi_ui_components.c`: estado Wi-Fi en header indica señal débil (RSSI menor a -76 dBm) sin inventar desconexión; iconografía de receptor y volumen ópticamente más fina.

## Criterios verificables

- Compilación host con `-Wall -Wextra -Werror` y vistas PPM a 320x240: correcto en este entorno.
- 5 pruebas host UI-06: pasan (regeneración idéntica, iconos distintos por tamaño, banda de animación acotada, pausa inmóvil, título UTF-8).
- El GIF es animación representativa del código C; NO es una grabación de hardware.
- Las pruebas de CPU/SPI/FPS/underrun I2S en ESP32-S3 real NO se han realizado aquí; no declarar cierre físico.

## Riesgos abiertos para próximo ciclo

- Wi-Fi exacto requiere contraste en panel físico; verificar 12px y 16px a distancia normal. Puede requerirse variante de alto contraste.
- La pantalla de diagnóstico concentra diez lecturas en dos columnas. Para inspección de errores difíciles, la WebUI de Micro Server debe ofrecer detalle remoto, contadores y trazas. No inferir éxito de descubrimiento del número de TX firmados.
- Rutas de pairing histórico que permanecen en el firmware requieren cierre arquitectónico aparte; NO se eliminaron en este sprint para no romper estado/FSM sin auditoría transversal.
- Medir visual refresh con y sin animación, tiempos de banda DMA, prioridad de tareas, jitter RTP, underruns I2S, estrés de Wi-Fi y transiciones de estado.
- Si el render es excesivamente costoso: bajar tasa UI con Kconfig, no el audio. Conservar apagado de animación como fallback.

## Archivos del sprint

- `firmware/components/michi_display/{michi_ui_icons.c,michi_ui_screens.c,michi_ui_components.c}`
- `firmware/components/michi_display/include/michi_ui_icons.h`
- `firmware/components/michi_display/tools/gen_precision_icons.py`
- `firmware/components/michi_display/assets/icons/michi_precision_icons_data.h`
- `tests/host/test_ui06_visual_contracts.py`
- `docs/ui/UI06_KILLCRITIC_FIFTH_PASS.md`
- `docs/ui/UI06_OPENCODE_VALIDATION.md`
- `docs/ui/previews-ui06/` (galería, comparativa, hoja óptica y animación)

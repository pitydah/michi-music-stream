# Michi Music Stream — UI-01 Quiet Hi-Fi (intervención en Google Drive)

**Fecha:** 2026-10-18  
**Estado:** Código fuente preparado para revisión y validación local. **NO** ejecutado ni certificado en hardware.  
**Pantalla:** Waveshare ESP32-S3-LCD-2, ST7789, 320 × 240, RGB565, sin interfaz táctil.  
**Alcance:** firmware/components/michi_display e instrumentos host de generación de previews.

## 1. Diagnóstico KILLCRITIC

- La antigua galería PNG no estaba sincronizada con los campos actuales del renderer (IP, reloj, discovery, metadatos) y documentaba pairing físico retirado. No usar estas capturas como representación fiel del firmware nuevo.
- `michi_ui_draw_header_landscape`, `michi_ui_draw_divider` y `michi_ui_draw_playback_footer_landscape` estaban definidos como primitivas, pero faltaba integrarlos coherentemente en el dispatcher de las pantallas normales. En la UI fuente se perdía la estructura visual de cabecera/pie que sugerían las previews antiguas.
- El texto `Mantén presionado el botón para comenzar` y el icono BUTTON eran incorrectos para la adopción por Wi-Fi/identidad preconfigurada desde Ecosystem.
- El diagnóstico decía `8 MB` de PSRAM cuando `ctx->psram_bytes` era cero/desconocido, aunque `michi_display.c` declara expresamente que pasa cero para evitar datos inventados.
- Fuentes actualmente rasterizadas a 1 bit e iconos de 1 bit; puede mejorarse el antialias con el soporte 2-bit existente, pero **no** se regeneran fuentes ni se toca el tamaño de firmware en UI-01.

## 2. Lenguaje visual adoptado

Nombre: **Quiet Hi-Fi**. Una pantalla de hardware debe comportarse como el panel de un componente audiófilo: tipografía clara, control de la saturación del color, superficies discretas, estados veraces y jerarquía inconfundible.

- Mantener el tema dark existente: `#080A0F` fondo, `#10141A` superficie, `#151A24` contorno suave, `#FF5C8A` acento Michi. Nunca introducir colores ad hoc sin pasar por la paleta semántica.
- **Sin simulación de transparencia, blur, cristal, sombras grandes, imágenes de stock o carátulas inventadas**. Las superficies se construyen con RGB565 sólido y contorno simple compatible con el renderizado por bandas DMA.
- Cabecera mínima: marca Michi a la izquierda; Wi-Fi y Micro Server en el borde derecho. Evitar duplicar la marca gigante en cada pantalla.
- Reproducción: estado en línea superior; título hasta 2 líneas en tipografía LG; artista una línea en MD; fuente debajo en una tarjeta de baja intensidad; pie con volumen real y formato negociado/validado.
- Prioridad visual: música > estado de reproducción > dispositivo fuente > formato > infraestructura de red. Información de red detallada solo en Diagnósticos.
- No añadir una falsa barra de progreso de pista: el renderer actual no dispone de posición/duración de reproducción autorizadas.
- No dibujar botones táctiles ni navegación que el hardware no puede ejecutar.

## 3. Cambios en el código fuente UI-01

1. `michi_ui_screens.c`: función `draw_surface` con recorte explícito a la banda DMA activa y contorno 1px; cero buffers o asignaciones dinámicas nuevos.
2. Nueva presentación compartida (`draw_playback_chrome` + `draw_track_info`) para PLAYING, BUFFERING, PAUSED; elimina duplicación de composición, respeta títulos UTF-8, dos líneas y elipsis del artista.
3. Cabecera/divisor estándar conectados al dispatcher en los estados normales, y en UPDATING. Pantallas críticas/overlays conservan precedencia existente.
4. Ready, Unprovisioned y Provisioning: jerarquía más legible, tarjetas sutiles y preparación desde Michi Ecosystem sin referencia al botón retirado.
5. Diagnósticos: superficie discreta, filas de 17px y PSRAM 'Desconocido' cuando no hay lectura real.
6. Footer: muestra volumen y formato con unidades en vez de un par de números ambiguos; si los campos son cero, muestra 'Audio sin confirmar'. Si falta etiqueta de la fuente, muestra 'Fuente no informada', nunca una sala inventada.
7. `render_ui_previews.c`: la muestra UTF-8 de referencia deja de aparentar soporte actual 96 kHz/24 bits; utiliza 48 kHz/16 bits, que corresponde a las capacidades anunciadas.

## 4. Restricciones de compatibilidad

- **No tocar audio, RTP, Wi-Fi, identidad Home ni sesiones**.
- **No crear imagen física falsa de producto ni afirmar Hi-Res, barra de progreso o carátulas sin metadatos reales**.
- Previsualizar exactamente desde las mismas fuentes C/headers que compila firmware, no mediante un diseño ficticio desconectado.
- Mantener compatibilidad con rendering de `320 × 240`, seis bandas `320 × 40` y comparación pixel-identical de host.
- Las rutas de pairing históricas permanecen en ciertas utilidades/test de regresión; retirarlas requerirá una migración posterior y no forma parte de UI-01.

## 5. Validación local necesaria (OpenCode/Codex)

**No ejecutada aquí por el flujo de trabajo Drive → descarga local → OpenCode.** Instrucciones:

1. Ejecutar `git diff --check`; confirmar cambios limitados a UI y renderizado de previews.
2. Compilar las pruebas host C que integran `michi_ui_screens.c`, `michi_ui_components.c` y fuentes de tipografía/iconos generados.
3. Ejecutar `test_michi_ui` verificando band identity 6×40/320×240, UTF-8, elipsis, prioridad de overlays, diagnósticos y resiliencia a contextos nulos.
4. Ejecutar el generador de previews host desde los fuentes actuales, exportar PNG RGB565 y reemplazar la galería `docs/ui/previews` de manera coherente (sin inventar soporte de audio); crear un apartado `legacy/` para pairing físico si el pipeline todavía lo genera.
5. Hacer revisión visual de textos largos, artistas UTF-8, ancho de fuentes, clipping entre y=175..225, estados PLAY/PAUSE/BUFFER y legibilidad desde 0,5–1 m.
6. Ejecutar build ESP-IDF v5.3, revisar uso de RAM/flash y tiempo de render; luego probar en placa real, incluidos artefactos ST7789 y tolerancias RGB565.
7. Registrar capturas físicas antes de declarar terminado UI-01. No dar por hecho que el equipo admite 2-bit fonts antes de presupuestar memoria/render y probar en hardware.

## 6. Fase siguiente — UI-02 acabado premium

- Regenerar solamente tipografías críticas con antialias 2-bit (comenzar por LG/MD) y medir incremento en flash/tiempo de render antes de adoptar masivamente. Confirmar correspondencia entre Kconfig de 2bit y assets generados.
- Refinar iconos de Wi-Fi, servidor, volumen, actualización y el emblema del gato con grosor óptico coherente a 12/16/32 px. No agregar stroke extra grueso.
- Rediseñar errores con acción concreta y código de diagnóstico, evitando afirmar disponibilidad de recuperación no implementada.
- Conectar UI de estado de Michi Home (configuración, confiable, no autorizada, sin red) a datos reales del firmware; no inventar estados.
- Si hay metadatos reales de posición/duración provistos y validados por Micro Server, diseñar progreso y tiempo con separación del progreso de buffer; mientras no existan, no mostrar.
- Actualizar documentación/fotografías del producto con diseño aprobado.

## 7. Condición de cierre

El diseño será **visualmente evaluable** tras generar la galería nueva en local y **certificable** solo con pruebas host, build y panel real. La intervención en Drive es una modificación de fuentes, no una validación funcional.

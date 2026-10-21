# Prompt para OpenCode — UI-04, validación posterior a reemplazar la carpeta descargada de Drive

Trabaja sobre Michi Music Stream. **NO rediseñes ni reescribas la UI.** La implementación UI-04 ya fue aplicada a los fuentes; tu tarea es verificar integración, corregir estrictamente fallos necesarios y certificar el resultado con evidencia objetiva. Preserva identidad gráfica, eventos de Michi Home, red y audio.

1. Comprueba `git status --short`, `git diff --check`, lista de archivos y origen de los cambios. No incluyas los artefactos históricos `build/`, `previews/` o `.git/` de Drive como cambios propios. Resuelve cualquier inconsistencia de Kconfig/sdkconfig (incluyendo la regeneración de sdkconfig si corresponde).
2. Compila **desde cero** con ESP-IDF para el perfil `esp32s3` y los componentes reales. Reporta IDF, versión toolchain, opciones de animación y memoria/flash consumida. No atribuyas errores preexistentes de otros módulos a UI-04 sin evidencia.
3. Construye el renderizador host de 320×240 y genera todas las variantes PPM con las seis bandas RGB565. Ejecuta `test_ui03_wave_previews.py` y `test_ui04_visual_invariants.py`. Verifica títulos largos, español UTF-8, pausa, buffering sin metadatos, animación fase 00/06, overlays, OTA, fatal y diagnósticos.
4. Verifica que `CONFIG_MICHI_DISPLAY_WAVE_ANIMATION=n` elimina todos los refrescos periódicos pero conserva los dibujos estáticos; con `y` mide funcionamiento con valores 160, 250 y 400ms. Revisa que el snapshot de fase sea único entre las seis bandas y no existan bloqueos del hilo I2S.
5. En placa real ST7789, medir con telemetría del equipo: periodicidad de repintado, tiempo render+flush, ocupación del bus SPI, uso de CPU, memoria libre, contadores I2S underrun, jitter, y errores de sesión. Comparar 10 min de reproducción con animación activada/desactivada bajo el mismo flujo RTP. Corregir exclusivamente si hay problemas reproducibles, sin tocar el motor de audio.
6. Presenta fotografías/videos de las pantallas reales y un informe con tablas PASS/FAIL, defectos visuales, diferencias respecto a las previews y estado de audio. No declarar DEVICE_E2E_PASS si no existe evidencia de placa y DAC real.

Restricciones: no retomar pairing físico, no inventar Hi-Res o datos de DAC, no falsear telemetría. Mantener trabajo sobre la rama elegida por el usuario y no publicar automáticamente si no lo solicita.

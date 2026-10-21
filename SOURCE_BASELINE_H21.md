# Michi Music Stream — Fuente única H21 (Drive)

**Fecha:** 2026-10-20. **Alcance:** reconciliación del árbol de código fuente en Google Drive; sin compilar, ejecutar tests ni flashear.

## Fuente de verdad

La carpeta original de Google Drive `michi-music-stream/` es la fuente de verdad. Su código de firmware conserva el baseline H16R con los cambios de ingeniería H18/H19, y las herramientas H20. Los ZIP H8–H20 son paquetes históricos o parches parciales: **NO aplicarlos como overlays a esta carpeta**.

## Reparación H21 efectuada

Se actualizaron *en el lugar* y conservaron sus IDs originales tres archivos cuyo contenido permanecía obsoleto:

- `README.md`: encabezado y descripción H19 actualizada.
- `docs/CURRENT_PRETEST_BASELINE.md`: baseline H19 coherente con la carpeta.
- `scripts/package_source_snapshot.py`: inventario H19 que incluye `michi_audio_diag.h` y el código/pruebas de seguridad Wi-Fi; es compatible con `tests/test_source_snapshot.py` ya presente.

Se releyeron los tres archivos desde Google Drive y se contrastó su contenido byte a byte con `MICHI_MUSIC_STREAM_H19_SOURCE_PATCH.zip`, con resultado idéntico. Ver `docs/H21_SOURCE_INTEGRITY.json`.

## Qué falta, sin confundir fases

- No se ha exportado desde Google Drive ni construido un ZIP completo de **todas** las fuentes; los ZIP anteriores siguen siendo parciales. El generador `scripts/package_source_snapshot.py` prepara un snapshot fuente íntegro, rechazando árboles incompletos. Ejecutarlo después de descargar la carpeta completa.
- No se han ejecutado tests, CI, ESP-IDF ni ensayos de DAC, Ethernet/Wi-Fi o Micro Server. Son una fase distinta que no computa en la finalización del trabajo en Drive.
- Puede haber defectos todavía no identificados: las comprobaciones de integridad de archivos no son certificación funcional.

## Proceso inequívoco de transferencia

1. Descargar **la carpeta completa original** de Google Drive, no un ZIP H15–H20.
2. Conservar el `.git` de la copia local existente y realizar copia de seguridad del árbol antes de reemplazarlo.
3. No ejecutar `reconcile_drive_sources.py --apply` sobre la copia nueva: los tres archivos ya fueron reconciliados en Drive.
4. Utilizar `scripts/package_source_snapshot.py` para preparar/inspeccionar una distribución fuente limpia; sus comprobaciones de completitud no sustituyen la validación de código.
5. Entregar a OpenCode/Antigravity la validación local descrita en `docs/OPENCODE_H19_LOCAL_VALIDATION.md` y `docs/OPENCODE_H20_VALIDATION.md`.

## Compatibilidad que se preserva

RTP PT97, PCM S16LE 48 kHz estéreo, 10 ms / payload de 1920 bytes, Michi Home, mDNS/Whisker y recuperación sin botón. No se promete salida analógica únicamente por haber admitido PCM en I2S.

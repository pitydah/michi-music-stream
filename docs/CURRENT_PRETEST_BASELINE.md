# Michi Music Stream — baseline de fuentes Drive H19

**Fecha local:** 2026-10-19. **Firmware de producción base:** H16R-PRETEST.
**Preparación adicional en Drive:** H18 (harness RTP y snapshot)
+ H19 (observación PCM, seguridad de secretos, política de red).

La carpeta ORIGINAL de Google Drive conserva el firmware H16R y recibe
las modificaciones H18/H19 en sus rutas normales. Los ZIP históricos son
piezas de auditoría/overlay y NO reemplazan por sí mismos el repositorio.

**Antes de sustituir local:** descargar la carpeta completa, respaldar
el árbol local, preservar `.git`, comprobar los hashes del último
manifiesto y usar el generador `scripts/package_source_snapshot.py`
para obtener un paquete integral limpio, si la descarga cumple todos
los requisitos. No superponer overlays antiguos sobre los archivos H19.

**Estado honestamente declarado:** fuentes H19 escritas, no se ejecutaron
builds ni tests, no hay certificación de ESP32-S3/PCM5122. El 100% de
Drive excluye pruebas locales/hardware, pero sigue requiriendo eliminar
brechas estáticas y contar con un snapshot integral contrastado.

Documentos vivos: `docs/H19_SOURCE_ENGINEERING_HANDOFF.md` y
`docs/OPENCODE_H19_LOCAL_VALIDATION.md`.

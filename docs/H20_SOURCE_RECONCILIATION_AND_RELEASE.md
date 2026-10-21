# Michi Music Stream — H20: Reconciliación segura de H19 y perfil de producción

**Tipo de trabajo:** desarrollo de fuentes en Google Drive. **Sin compilar, ejecutar tests, flashear ni realizar E2E.**

## Diagnóstico verificado

Al revisar la carpeta original se identificó una discrepancia heredada de la publicación H19:

- `scripts/package_source_snapshot.py` original: H18 (lista de archivos obligatorios incompleta para H19).
- `tests/test_source_snapshot.py` original: H19, exige `michi_audio_diag.h`, que no aparece en el inventario del script H18.
- `README.md` original: cabecera H15.
- `docs/CURRENT_PRETEST_BASELINE.md` original: cabecera H16R.

Los archivos H19 completos existen en `docs/MICHI_MUSIC_STREAM_H19_SOURCE_PATCH.zip`, cuyo manifiesto indica los SHA256 esperados. La incompatibilidad **bloquea el cierre estático del 100% si se usa solamente la carpeta Drive sin reconciliar**.

## Nuevas piezas de ingeniería

1. `scripts/reconcile_drive_sources.py`: verificación de estructura, hashes por archivo, manifest/ZIP exhaustivo, protección contra path traversal, symlinks y colisiones, guardia de versiones conocidas, modo lectura predeterminado y corrección opcional de **solo tres rutas históricamente rechazadas**. Exige copia de seguridad en carpeta externa y no modifica `.git`.
2. `tests/test_reconcile_drive_sources.py`: casos para integridad, coincidencia, rechazo de versiones desconocidas, protección de ZIP y backups, aplicabilidad exacta e idempotencia.
3. `scripts/check_release_profile.py`: auditoría estática de configuración de producto y separación de redes de laboratorio frente a políticas de producción.
4. `firmware/sdkconfig.production.defaults`: overlay opcional de firmware de producción que exige WPA2 o superior y desactiva BLE de laboratorio; no altera el build base.
5. `tests/test_release_profile.py`: pruebas de reglas de release y configuración contradictoria.

## Uso después de descargar la carpeta completa

1. Preservar `.git` local y respaldar el repositorio anterior.
2. Aplicar los archivos de este paquete H20 en una copia de trabajo. Mantener el ZIP oficial H19 en `docs/`.
3. Precomprobación sin modificar el árbol:
   `python scripts/reconcile_drive_sources.py --root .`
4. Si los **únicos** pendientes son los tres stale known-good y no hay cambios ajenos:
   `python scripts/reconcile_drive_sources.py --root . --apply --backup-dir ../michi-h20-backup-unique`
5. Repetir el comando en modo lectura (sin `--apply`) y exigir que `complete` sea `true`. Comprobar después el snapshot completo:
   `python scripts/package_source_snapshot.py --root . --out ../michi-stream-source.zip --baseline H20-SOURCE-PRETEST`
6. Revisar perfil de producción:
   `python scripts/check_release_profile.py --root . --profile production`
7. Recién entonces ejecutar los tests/ESP-IDF con OpenCode o Antigravity (fase aparte).

**No se debe aplicar automáticamente H19 a todos los archivos**: la mayoría ya fue actualizada directamente en Drive. La herramienta de reconciliación se detiene si encuentra una versión inesperada en cualquier entrada del ZIP; no pretende resolver conflictos Git arbitrarios.

## Separación de alcances

Dentro de Drive: código fuente, pruebas redactadas, coherencia entre archivos, herramientas reproducibles y documentación.

Fuera del porcentaje Drive: ejecución de pytest/host/CI, idf.py, flasheo, DAC/PCM5122, Wi-Fi real y convergencia con Michi Micro Server/WebUI.

**Estado:** H20 mejora las herramientas de release y resuelve de manera documentada el procedimiento seguro para reconciliar H19 **después de la descarga**. No afirma que los originales rechazados de Drive hayan sido sustituidos. Tampoco representa una distribución integral descargable por sí sola.

# Michi Music Stream — H22, descarga de carpeta Drive y reemplazo local

**Fecha de baseline:** 2026-10-20. **Estado:** código fuente PRETEST, sin compilación ni tests ejecutados por ChatGPT.
**Fuente de verdad:** la carpeta original `michi-music-stream/` de Google Drive; las modificaciones H22 están incorporadas en sus archivos individuales (no solo en el ZIP).

## Procedimiento recomendado — descargar carpeta completa

1. En Google Drive, descarga la **carpeta original `michi-music-stream/` completa**. Extrae el ZIP que genera Google en un directorio temporal en tu PC. No descargues solamente los ZIP históricos de `docs/`.
2. Identifica la ruta de ese directorio descargado, por ejemplo `/home/usuario/Descargas/michi-music-stream`. La carpeta contiene archivos fuente más artefactos de desarrollo históricos; NO los copies indiscriminadamente al checkout local.
3. Conserva una copia de seguridad de tu repositorio local; **no reemplaces su `.git`**. Tus credenciales, NVS y configuraciones privadas tampoco deben entrar en la transferencia.
4. Haz una inspección *sin modificar nada* (sustituye ambas rutas):

```bash
python3 /ruta/descargada/michi-music-stream/scripts/install_drive_sources.py \
  --from-drive /ruta/descargada/michi-music-stream \
  --to-local /ruta/a/tu/repo/michi-music-stream
```

5. Solo si el inventario y la cantidad de archivos son coherentes, aplica la sustitución:

```bash
python3 /ruta/descargada/michi-music-stream/scripts/install_drive_sources.py \
  --from-drive /ruta/descargada/michi-music-stream \
  --to-local /ruta/a/tu/repo/michi-music-stream --apply
```

El script verifica **SHA-256 y tamaño de cada archivo fuente** usando `docs/H22_SOURCE_TREE_MANIFEST.json`, se niega a proceder ante fuentes incompletas, conserva `.git`, excluye compilados/ZIP históricos/credenciales, copia solo fuentes cambiadas o faltantes, y realiza respaldo de los archivos locales reemplazados. No elimina archivos locales extra; verifica posteriormente `git status` y `git diff` con OpenCode. En la revisión H22 se corrigió el tratamiento de permisos ejecutables de `scripts/*.sh` y se eliminaron errores estructurales de tres casos de pruebas fuente. Estas correcciones siguen pendientes de ejecución local. Si hay un fallo de escritura a mitad de la operación, recupera los ficheros anteriores desde la carpeta de respaldo notificada.

## Archivos H22

- `START_HERE.md`: referencia principal para descargar la carpeta completa.
- `docs/H22_SOURCE_TREE_MANIFEST.json`: inventario verificable de la copia fuente H22, autocontenido excepto su propio hash.
- `scripts/install_drive_sources.py`: importación segura en checkout Git local, vista previa por defecto.
- `scripts/package_source_snapshot.py`: empaquetador reproducible con componentes obligatorios.
- `scripts/verify_source_snapshot.py`: verificador de ZIP alternativo.
- `tests/test_source_snapshot.py`: pruebas fuente preparadas para ejecutar localmente.
- `docs/H22_OPENCODE_LOCAL_VALIDATION.md`: comandos de pruebas y flasheo posteriores en OpenCode.
- `docs/MICHI_MUSIC_STREAM_H22_COMPLETE_SOURCE_PRETEST.zip`: ZIP histórico del mismo baseline como descarga alternativa; los archivos en las rutas originales de Drive son la fuente a utilizar.

## Límites de certificación

La preparación de fuentes y sus SHA-256 NO prueban que ESP-IDF compile ni que los tests pasen. **NOT RUN:** pytest, tests C host, CI, builds ESP-IDF, ESP32-S3/PCM5122, Micro Server y WebUI. Sigue vigente Michi Link alpha.5 y el perfil RTP PT97/PCM S16LE 48 kHz estéreo, 10 ms, 1920 bytes. `pcm_packets_submitted` no demuestra salida analógica audible.

# H22 — Validación local para OpenCode (solo después del reemplazo)

**Esta documentación prepara la validación; nada se ejecutó desde Google Drive.**

0. Descargar la carpeta completa de Google Drive; usar `scripts/install_drive_sources.py` en modo inspección y luego `--apply`, siempre preservando el `.git` local y respaldando secretos/NVS. Verificar `docs/H22_SOURCE_TREE_MANIFEST.json` antes de compilar.
1. `git status --short`, `git diff --check` y revisión humana del diff respecto a main.
2. `python3 scripts/reconcile_drive_sources.py --root .` en modo auditoría, **nunca `--apply`** sobre H22.
3. `python3 scripts/check_release_profile.py --root . --profile lab`; revisar production sin habilitar parámetros inseguros durante la primera prueba.
4. `python3 -m pytest tests/test_source_snapshot.py tests/test_release_profile.py tests/test_reconcile_drive_sources.py -q`. Confirmar que las regresiones de `test_source_snapshot.py` se ejecutan correctamente tras corregir el inventario de rutas requerido, la eliminación de carpetas no vacías y la prueba de componentes con subdirectorios.
5. `make -C tests/host clean && make -C tests/host test`. Ninguna omisión automática de libcjson. Pruebas del RTP real, audio diagnostics y secretos Wi-Fi obligatorias.
6. `./scripts/run_tests.sh --full`. Inspeccionar reportes contract/e2e y compatibilidad efectiva con Michi Link alpha.5.
7. Compilar `idf.py build` en ESP-IDF compatible con ESP32-S3, perfil Standard y Hi-Fi. Validar include paths, versión ESP-IDF, flags Kconfig y tamaño de particiones.
8. Validar en placa: provisioning de fábrica MICHI-F1 autorizado, Whisker/mDNS, desafío Home, sesión Micro Server, RTP PT97 10ms 1920B, salida I2S/PCM5122 analógica, PAUSE/STOP, reconexión y cierre de fallos. Incluir evidencia en WebUI de Micro Server en el mismo sprint.
9. OTA/rollback, recuperación USB autenticada, pérdida de Wi-Fi, fail-closed y secretos con hardware real.

En un fallo: registrar comando, commit, líneas de error, causa y corrección concreta. Nunca reducir asserts, sustituir el motor real por fake ni anunciar PASS no observado.

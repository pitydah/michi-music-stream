# OpenCode / Antigravity — Validación H18 de fuente real RTP (solo etapa local)

La carpeta Google Drive fue preparada SIN EJECUTAR tests ni compilación. Preservar el `.git` local, hacer backup y trabajar sobre baseline H16R + fuentes H17/H18.

1. Revisar `git status`, diffs y `docs/H18_SOURCE_MANIFEST.json`; comparar 3 archivos existentes sustituidos con manifiesto. No aplicar H15 alternativo histórico.
2. Ejecutar únicamente después de descargar: `make -C tests/host clean && make -C tests/host test_michi_audio_engine`, y corregir **en fuente** cualquier error C11 de `sdkconfig`, cabeceras lwIP de POSIX, shim RTP, `esp_timer`, I2S fake, `-Werror` o enlazado. No marcar PASS antes de ejecutar.
3. Repetir varias veces `tests/host/test_michi_audio_engine` con límites de ejecución; observar UDP real, PAUSE dentro de recvfrom, timer monotónico, creador/task join, SSRC, memoria, fault injection de bind/alloc/xTaskCreate y quiesce. Si hay inestabilidad, hacer scheduler gates deterministas en el shim; no ocultarla.
4. Ejecutar `python3 -m pytest tests/test_source_snapshot.py -q`, con tests de directorio y archivos obligatorios; generar snapshot fuente SOLO cuando Drive descargado incluya realmente todo y contenga cero material sensible.
5. Ejecutar suite completa `./scripts/run_tests.sh --full` y `idf.py build` dentro de `firmware`. Reportar stdout/stderr, códigos de salida, warnings, hashes, versión exacta y todos los fallos. No borrar NVS ni flashear sin permiso.
6. Mantener certificados de hardware e interop Micro Server separados del cierre estático de Drive. Los tests HOST no son prueba del PCM5122 físico.

CRITERIOS: G1 RTP real/host, G2 tests de snapshot, G3 suite total host/contract/e2e, G4 ESP-IDF build, G5 físico/interop externo, todos con PASS/FAIL/BLOCKED según ejecución. No repetir promesas de 100% sin evidencia.

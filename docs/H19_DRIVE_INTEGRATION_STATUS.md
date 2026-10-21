# H19 — Estado exacto de integración en Google Drive

Fecha local: 2026-10-19. Este documento registra la publicación del H19 sin declarar PASS de compilación, test o dispositivo.

## Archivos H19 publicados en rutas originales

- `firmware/components/michi_audio/michi_audio.c` (contadores de entrega al ring)
- `firmware/components/michi_audio/include/michi_audio.h` (métricas)
- `firmware/components/michi_audio/include/michi_audio_diag.h` (nuevo)
- `firmware/components/michi_http/http_server.c` (`audio_observation`)
- `firmware/components/michi_wifi/michi_wifi.c` (borrado de secretos, log discreto y política Wi-Fi)
- `firmware/components/michi_wifi/include/michi_wifi_secret.h` (nuevo)
- `firmware/components/michi_wifi/Kconfig` (opción WPA2)
- `tests/host/Makefile` (nuevos objetivos)
- `tests/host/test_michi_audio_engine.c` (regresión de PCM)
- `tests/host/test_michi_audio_diagnostics.c` (nuevo)
- `tests/host/test_michi_wifi_secret.c` (nuevo)
- `docs/ROADMAP.md` y documentos H19 nuevos.

## Actualizaciones rechazadas por controles del servicio

- `README.md`: el original sigue con el resumen anterior (H15); el README H19 revisado está dentro del ZIP.
- `docs/CURRENT_PRETEST_BASELINE.md`: el original sigue designando H16R; el baseline H19 revisado está dentro del ZIP.
- `scripts/package_source_snapshot.py`: el original continúa con los requisitos H18; la ampliación H19 está dentro del ZIP.
- `tests/test_source_snapshot.py`: sí se actualizó con una prueba adicional que espera la ampliación H19 del generador; su restauración posterior fue también rechazada. **Por tanto, el árbol Drive sin overlay H19 puede FALLAR esa prueba de snapshot hasta reconciliar ambos archivos. No declararlo PASS ni 100%.**

## Fuente de verdad para el handoff

El paquete `docs/MICHI_MUSIC_STREAM_H19_SOURCE_PATCH.zip` y su `docs/H19_SOURCE_MANIFEST.json` contienen el conjunto H19 coherente de los 18 archivos cambiados (más manifiesto), pero son un **overlay**, NO un repositorio completo. El resto del firmware proviene de H16R y las fuentes H18 actuales en la carpeta original.

Esta diferencia es un bloqueante explícito del cierre final de Drive. La integración debe verificarse/reconciliarse antes de sustituir la copia local. Tras descargar Drive, superponer H19 en una copia de trabajo resuelve esos cuatro archivos fuente/documentación, conservando `.git` local. No ejecutar validaciones en Drive; OpenCode/Antigravity realizará el build/test físico después.

# H19 — validación local después de descargar Google Drive (OpenCode/Antigravity)

La copia de Drive no fue compilada ni probada. Mantener el árbol local `.git` y respaldar identidad/NVS antes de cualquier flash.

1. Revisar SHA del manifiesto H19, SHA de archivos fuente y `git diff --check`.
2. Construir y ejecutar `make -C tests/host clean && make -C tests/host test` y `./scripts/run_tests.sh --full`, sin omitir objetivos por ausencia de libcjson.
3. Ejecutar `test_michi_audio_diagnostics`, `test_michi_wifi_secret` y `test_michi_audio_engine` enlazado al RTP REAL. Comprobar que no hay mismatches de struct en compilación, ni duplicidad de símbolos host y que cada contador PCM concuerda con admisiones reales.
4. Hacer `idf.py build` con ESP-IDF del proyecto; verificar inclusión de `michi_audio_diag.h` desde el servidor HTTP y `michi_wifi_secret.h` desde Wi-Fi, con la configuración default y con `CONFIG_MICHI_WIFI_REQUIRE_WPA2=y`.
5. Validar API diagnósticos en simulación: raíz antigua sin cambios, bloque `audio_observation` solo software, sin tokens/secrets; validar paused/buffering/fault e intercalaciones STOP/START.
6. Mantener las pruebas físicas de PCM5122, reproducción prolongada y aceptación de Micro Server/WebUI como fase **separada** de la preparación de fuentes Drive.

Si una puerta falla, registrar comando, salida y corrección concreta; **no declarar PASS por estar redactada una prueba**.

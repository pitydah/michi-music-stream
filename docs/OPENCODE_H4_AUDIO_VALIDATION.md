# Prompt para OpenCode — Certificación local H4 Audio

Trabaja en el repositorio local de **michi-music-stream** después de incorporar desde Google Drive la pasada H4. Haz pruebas y correcciones sobre el código local. Mantén el bundle `contracts/michi-link` intacto. **No modifiques Michi Ecosystem**, que se trabaja en paralelo.

1. Usa el repositorio Git local como fuente de autoridad del historial. Muestra primero `git status --short`, `git diff --stat`, el commit HEAD y las rutas exactas recibidas. No reemplaces `.git` con la copia de Drive.
2. Revisa `docs/H4_AUDIO_RELIABILITY_HANDOFF.md` y la diferencia de `michi_audio.c`, `michi_jb.c`, `michi_jb.h`, `Kconfig`, `test_michi_jb.c`. Verifica la derivación de memoria `500/10 * 1920 = 96000` bytes.
3. Compila y ejecuta la suite host con `./scripts/run_tests.sh --full` o con su runner vigente, sin reutilizar ejecutables precocinados. Informa número de pruebas y fallos. Agrega tests de límite para inserción oversized con buffer lleno; wrap 65535→0; no silencios de 65535 paquetes. Prueba ASan/UBSan en host si el entorno lo soporta.
4. Compila ESP-IDF para la placa Waveshare ESP32-S3-LCD-2 sin cambiar el contrato de 48kHz, 16 bits, estéreo PT97 ni el transporte de 10 ms. Reparar cualquier warning/error introducido en H4.
5. Ejecuta regresiones de la UI, HTTP, Michi Home, RTP, I2S y las pruebas existentes; investiga cada fallo y clasifícalo como H4, preexistente o indeterminado.
6. Si hay hardware disponible: valida SDRAM/PSRAM, contadores de jitter, underrun, recepción RTP, comportamiento con paquetes fuera de orden, PAUSE/RESUME, Wi-Fi desconectado, DAC PCM5122 y tras 24 horas. Si no lo hay, deja `DEVICE_E2E_NOT_TESTED`, sin inventar resultados.
7. No cambies RFC RTP ni reduzcas packetization a 5 ms unilateralmente. La solución a fragmentación IP requiere modificaciones y aprobación conjunta con Micro Server.
8. Entrega informe markdown con resultados, diff, defectos y blockers, y commit local de correcciones solo después de pasar los gates pertinentes. No declares 100% por aprobar pruebas host.

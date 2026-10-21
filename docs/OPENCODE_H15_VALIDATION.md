# OpenCode / Antigravity — validación local H15 (solo después de reemplazar la carpeta)

Contexto: el código fue editado en copia Drive sin ejecutar pruebas.
NO aceptar declaraciones de PASS basadas en presencia de test o ZIP válido.
Trabaja en tu copia local conservando `.git`; verifica H15_SOURCE_MANIFEST.json.

## Puertas y acciones obligatorias

1. `git status --short`, `git diff --check`, comparar hash del overlay H15.
2. `make -C tests/host clean && make -C tests/host test` y
   `./scripts/run_tests.sh --full`. `libcjson` obligatorio.
3. `cd firmware && idf.py build` con IDF 5.3 (ESP32-S3); investigar
   `_Atomic` Xtensa y `esp_timer_restart`, FreeRTOS, niveles warnings/errors.
4. Compilar y probar la política H15 y las cuatro regresiones de sesión.
5. Validar PAUSE mientras `recvfrom` bloquea y QUIESCE se confirma,
   PAUSE/RESUME rápidos, STOP durante backpressure, y DMA FAULTED.
6. Comprobar runtime de timer y rollback: `esp_timer_restart()` mientras
   activo, fallo inyectado, secuencia heartbeat intacta, no sesión zombie,
   watchdog autónomo sin nuevas peticiones HTTP, y saturación de cola.
7. Añadir prueba que compile el motor RTP **real** en harness host, ya que
   `test_michi_session.c` solo utiliza fake RTP; ejecutar y corregir.
8. Revalidar HTTP 500 watchdog, canonical JSON y respuestas de simulador.
9. Revisar leaks (PSRAM, socket, semáforos), estado I2S y eventos FSM,
   fail-closed Home, anuncios mDNS/Whisker, watchdog y recuperación OTA.
10. Ejecutar en físico ÚNICAMENTE en etapa autorizada: ESP32-S3 PCM5122,
    claves, descubrimiento, RTP, 24h, interrupciones Wi-Fi, rollback.

Reportar por archivo: SHA, compilación, suite, error reproducible, diff,
logs y corrección. No push sin autorización; no borrar NVS ni identidad.
Los cambios a Micro Server deben acompañarse de WebUI en ese repositorio.

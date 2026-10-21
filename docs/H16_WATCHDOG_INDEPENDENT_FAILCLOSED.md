# H16 — Autonomía de expiración de lease: revisión de fuente

**Base:** H15-PRETEST, árbol original de Google Drive (fuentes en ubicación original). **Tipo:** fuente sin ejecución de tests.

## Problema corregido
El watchdog de H15 podía perder expiración autónoma si el callback de `esp_timer` no conseguía enviar a la cola y `esp_timer_start_once` del reintento fallaba. El lease seguía venciendo en tiempo monotónico pero sin evento, hasta la siguiente consulta HTTP. Eso no es suficiente para una sesión que debe caducar aunque el controlador desaparezca.

## Diseño H16
- `session_task_func()` recibe en la cola con espera FINITA de 1000 ms (10 ms bajo `MICHI_HOST_TEST`). Al vencer la espera, compara tiempo monotónico real y deadline del lease bajo `s_mutex`. Si venció, invoca la misma función de teardown que la ruta de evento; sin necesidad de HTTP ni callback del timer.
- Un segundo fallo de cierre no borra credenciales mientras el motor siga vivo. La comprobación se repite en el próximo ciclo de vigilancia.
- El callback sigue siendo no bloqueante; no realiza I2S/teardown desde la tarea de `esp_timer`.
- `tests/host/shim/freertos/queue.c` implementa espera con timeout real y conserva `portMAX_DELAY` ilimitado, permitiendo que la simulación de watchdog sea representativa.
- El shim incluye inyección de cola llena `test_queue_set_force_send_fail` y temporizador `test_esp_timer_fail_next_start`. El reloj monotónico del fake es ahora `_Atomic int64_t` para permitir avance en un hilo y lectura desde el worker sin carrera de datos.
- `test_h16_autonomous_lease_without_timer_delivery()` valida falla combinada, vigila el contador de expiración AUTÓNOMA antes de llamar a la API y comprueba que una sesión nueva puede comenzar después.

## Validación posterior (NO ejecutada)
1. Revisar pthread condvar timed waits, C11 atomics, timeouts de FreeRTOS y unidad de `pdMS_TO_TICKS`.
2. Ejecutar suite host completa con build limpio, regresión H16, pruebas de I2S H8-H14, `.github/workflows/ci.yml` y ESP-IDF build.
3. Inducir saturación de cola y fallo de timer, y comprobar que el lease se invalida sin tráfico HTTP en placa real.
4. Probar que el watchdog de 1s no causa problemas de watchdog de tarea, core pinning ni memoria, y que un teardown de 2s no interfiere con el scheduler.

## Bloques de trabajo estático todavía abiertos para el 100% Drive
- Harness que compile y ejercite `michi_audio.c` RTP REAL con socket y DMA inyectados.
- Diagnóstico de transporte/audio semánticamente verificable (no inferir analog signal de I2S RUNNING).
- Auditoría exhaustiva de recuperación Wi-Fi/Home/NVS, secreto/OTA, contratos y policy SKU.
- Entrega única de árbol fuente completo, limpio y versionado; no depender de overlays acumulativos.

**Estado H16:** 6 fuentes/shims/tests preparados para pruebas locales. No es certificación ni cierre del 100% de Drive.

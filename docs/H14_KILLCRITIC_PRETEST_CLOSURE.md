# Michi Music Stream — H14: cierre de preparación pretest

**Fecha:** 2026-10-19. **Base:** paquete acumulativo H13 (24 archivos) y su árbol fuente vigente en Drive.

## Cambios H14 aplicados al código

1. **RTP task startup handoff** (`firmware/components/michi_audio/michi_audio.c`): la tarea RTP se bloquea en `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)` hasta que el creador publique su `TaskHandle_t` y marque la sesión activa. Luego recibe `xTaskNotifyGive(task)`. Esto elimina la ventana H13 *check done -> write handle*, que seguía siendo una carrera lógica aunque las variables fueran `_Atomic`.
2. **Preparación transaccional de sesión RTP**: primero se reserva el contexto, jitter buffer, buffers de recepción/silencio y se hace `bind()` de UDP. Solo después se ejecuta STOP/START de la salida I2S. Un error de `malloc`/PSRAM/bind no reinicia innecesariamente el DAC. Si falla la transición I2S, se libera el socket y toda la memoria; no se anuncia sesión.
3. **Ownership común de recursos RTP**: `session_context_release()` centraliza liberación de socket, jitter pool y buffers tanto en rechazo de startup como en salida del worker. Tras fallar `xTaskCreate`, se revoca la sesión y se intenta barrera de silencio; el error de rollback se registra sin ocultar la causa original.
4. **Estado `session_active` veraz**: solo se publica después de la creación efectiva de la tarea y antes de liberarla, evitando que la API observe una sesión activa que nunca pudo arrancar.
5. **Deadline PCM de tiempo total** (`michi_audio_output.c`): se usa `xTaskGetTickCount()` y diferencia modular de `TickType_t`; la duración completa del productor tiene límite, aunque avance parcialmente cada cierto tiempo. Previamente el contador sumaba solamente vueltas sin avance y podía exceder el presupuesto real. Se conserva `vTaskDelay(1)` para prevenir starvation.
6. **Host FreeRTOS shim**: `task.h` y `task.c` incluyen `xTaskGetTickCount()` con CLOCK_MONOTONIC; no se emplea el reloj civil para medir deadlines.
7. **Regresión redactada**: `test_h14_total_pcm_write_deadline()` en `test_michi_audio_output.c` simula un I2S lento que consume PCM a intervalos y exige que la escritura grande termine por `ESP_ERR_TIMEOUT` dentro del presupuesto más tolerancia. No se ha ejecutado.

## Invariantes deliberadamente preservados

- RTP PT 97, S16LE 48k/16/2, 10ms, payload 1.920B, IP/SSRC negociados, único productor RTP y único worker I2S.
- Michi Link/Home, membresía, credenciales, NVS, BLE, Wi-Fi, mDNS, Whisker, API HTTP, OTA y UI permanecen iguales.
- No pairing físico. `now_playing` sigue experimental; no se proclama soporte nuevo.
- No se cambia solo en Stream el contrato RTP: el reensamblado IPv4 existente necesita pruebas con Micro Server.

## Límites de la entrega (no confundir código con certificación)

- No se ejecutaron compilación ESP-IDF, tests host, simulador, CI ni placa ESP32-S3, conforme al flujo del usuario. Un informe OpenCode/Antigravity debe descubrir y arreglar fallos compilatorios o de comportamiento si los hay.
- `tests/host/test_michi_session.c` compila el módulo de control real con un **fake del motor RTP**: NO prueba directamente `michi_audio.c`. Es requisito crear una prueba del motor RTP real mediante shim de sockets/tareas o integración E2E. La regresión nueva H14 cubre solamente el deadline PCM.
- `xTaskNotifyGive` de arranque utiliza el notification slot de tarea RTP: verificar en build/inspección que ninguna otra función utilice ese mismo slot para otro protocolo. Si en el futuro se añade otra notificación, emplear índices explícitos (`xTaskNotifyIndexed`).
- OTA A/B/rollback, recuperación USB autorizada, fragmentación RTP, confianza Home/Micro Server/WebUI y DAC PCM5122 deben certificarse sobre dispositivos.
- El ZIP es **un parche acumulativo** de rutas H8–H14; no contiene el repositorio completo. La copia íntegra se obtiene descargando la carpeta raíz de Drive. Preservar `.git` local.

## Criterios de rechazo inmediato

- Compilación Xtensa o host falla; H14 queda abierto hasta corregir.
- Inicio de sesión RTP reporta activo sin socket/tarea/propiedad válidos.
- Algún estado STOPPED, QUIESCED o sesión saludable se anuncia después de un fallo I2S no reconciliado.
- El deadline de productor no se cumple, regresión de pausa/STOP o use-after-free.
- Conexión Home falsa, datos de identidad descartados, WebUI muestra streaming sin evidencia.

**Estado formal:** código fuente H14 aplicado; empaquetado y hash comprobables; build/tests/hardware PENDIENTES. No es una certificación 100% funcional.

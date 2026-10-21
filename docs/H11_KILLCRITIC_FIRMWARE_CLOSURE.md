# Michi Music Stream — H11 KILLCRITIC: ciclo de vida I2S y propiedad del PCM

**Base:** H10 de Google Drive, 19 de octubre de 2026. **Tipo:** refactorización de fuentes y regresiones, *no certificación*. **Estado de validación:** NO EJECUTADA (tests, CI, build ESP-IDF, placa). **Objetivo de producto:** elevar la preparación técnica hacia el umbral interno del 85 % **solo después de verificar los criterios de aceptación**; no equiparar ese porcentaje a reproducción física demostrada.

## Problemas prioritarios corregidos en código

1. **H11-P0-01 — Carrera START/STOP/DEINIT/INIT.** El `s_state_lock` anterior se liberaba antes de habilitar o destruir el canal I2S; otras tareas podían iniciar un nuevo ciclo sobre el mismo handle. Nueva *lifecycle lease* estática y no bloqueante: una única llamada `init/start/stop/deinit` puede realizar transiciones y operaciones del driver a la vez. Una segunda llamada devuelve `ESP_ERR_INVALID_STATE`, con obligación de reintento por el llamador. **No se sostiene ningún spinlock durante operaciones del driver.**
2. **H11-P0-02 — Productor PCM tardío vs. DEINIT/START.** Antes, `write()` podía superar su primera comprobación de RUNNING, quedar suspendido, y luego encontrar el ring ya liberado o reutilizado. Se introduce `s_write_inflight` bajo `s_state_lock`: todo `write()` admitido adquiere y libera una referencia. DEINIT no puede liberar el ring y START no puede reciclarlo mientras haya un productor anterior vivo. Se conserva la regla SPSC: un solo productor; no se autoriza multiwriter.
3. **H11-P0-03 — STOPPED prematuro.** El worker autoeliminado ya no publica STOPPED; solo STOP puede hacerlo una vez verificada la deshabilitación del canal I2S. El worker conserva STOPPING o publica FAULTED ante una salida no explicada. Los reintentos tras error de `disable` se determinan por la propiedad explícita `s_channel_enabled`, incluso si el primer STOP terminó en timeout de join.
4. **H11-P0-04 — Fallo al eliminar un canal durante INIT.** Cuando `i2s_channel_init_std_mode`, la creación de semáforos o la configuración de volumen fracasan, el módulo intenta eliminar el canal; si esa eliminación también falla, **NO libera PSRAM ni pierde el handle**. Marca el módulo en FAULTED, conserva la propiedad y solo permite reintento de DEINIT. Si la eliminación tiene éxito, libera el ring como antes.
5. **H11-P1-05 — Tarea que termina antes de publicar `s_task`.** START detecta que el worker se autoeliminó entre `xTaskCreate` y la publicación del handle, evitando resucitar una referencia obsoleta. Devuelve error; STOP es responsable de reconciliar la deshabilitación.
6. **H11-P1-06 — Test determinista de concurrencia.** Shims del host permiten demorar artificialmente enable/delete I2S, esperar la entrada del driver e inyectar errores. El código de producción no compila esas demoras.

## Archivos modificados

- `firmware/components/michi_audio_output/michi_audio_output.c`
- `firmware/components/michi_audio_output/include/michi_audio_output.h`
- `tests/host/test_michi_audio_output.c`
- `tests/host/shim/i2s_shim.c`
- `tests/host/shim/driver/i2s_std.h`

## Compromisos de compatibilidad

- No se cambian Michi Link, Home, OTA, Whisker, mDNS, Micro Server ni Ecosystem.
- No se modifica el contrato de transporte: RTP PT97, S16LE 48 kHz, dos canales, 10 ms y payload PCM de 1920 bytes. El problema de fragmentación/MTU permanece **P0 pendiente de coordinación entre repositorios**.
- No se altera la interpolación de volumen y mute H7/H8, la política del jitter buffer H6 ni las métricas H9.
- H11 tiene semántica `try-enter` para operaciones de ciclo de vida. Los llamadores que dependen de llamadas concurrentes deben reintentar o secuenciarlas; revisar especialmente `michi_session.c` y los callbacks HTTP durante la validación.
- El estado I2S enabled es un indicador de aceptación del driver, **no confirma que el DAC emita audio analógico**.

## Pruebas de regresión añadidas: fuente redactada, SIN EJECUTAR

- Fallo compuesto `i2s_channel_init_std_mode` + `i2s_del_channel`; fuga retenida, START/INIT rechazados y recuperación DEINIT.
- START en mitad de un `i2s_channel_enable` demorado; STOP, START y DEINIT concurrentes rechazados sin tocar el driver.
- DEINIT en mitad de un `i2s_del_channel` demorado; INIT/START concurrentes rechazados hasta liberación.
- Fallo de disable + timeout de join con worker retenido; STOPPING permanece hasta reintento exitoso y no publica STOPPED antes de deshabilitar.
- Writer de PCM retenido tras ser admitido: STOP puede cerrar el worker, pero DEINIT y nuevo START rechazan acceso mientras exista productor in-flight; el writer recibe estado inválido al terminar.

## Riesgos de implementación a revisar con OpenCode

- Semántica ESP-IDF real de los errores de disable/delete, especialmente canal deshabilitado en el driver pero con error retornado (los shims modelan un estado conservador, no hardware real).
- Memoria del task FreeRTOS entre `s_task_done=true` y `vTaskDelete(NULL)`; auditoría de ausencia de uso posterior de recursos compartidos.
- Dependencias del nivel de sesión: qué hace Micro Server cuando START o STOP retorna ESP_ERR_INVALID_STATE temporal por contención.
- Estrategia de registro `s_write_inflight` en caso de reset/abort del task productor por razones externas; no hay cancelación forzada.
- Resultados con `CONFIG_FREERTOS_HZ=100` y 1000, PSRAM habilitada, ESP32-S3, PCM5122, interrupciones reales y pérdidas Wi-Fi.
- Realizar revisión estática adicional de `michi_session` al integrar el parche y descartar reentrancias de API fuera del contrato.

## Puerta de salida (NO CUMPLIDA)

H11 solo es candidato para avanzar al **85% de preparación de firmware** si pasan: diff e integración H1–H11, host completo y regresiones nuevas, build ESP-IDF clean, validación de concurrencia, revisión de callers y ausencia de P0 de seguridad de memoria. Ese 85% **no** significa hardware/OTA/Home/Micro Server certificado. El ensayo físico y E2E se realizan después y tienen puertas propias.

**Rollback:** revertir los cinco archivos como una unidad si aparece una regresión y preservar `.git`, NVS, claves y demás módulos. No realizar flasheo hasta después de completar build y validar el comportamiento local.

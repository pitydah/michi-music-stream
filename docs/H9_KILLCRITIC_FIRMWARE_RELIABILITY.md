# MICHI MUSIC STREAM — H9 KILLCRITIC: estabilización I2S, diagnóstico y convergencia H8

Fecha: 2026-10-19. Base: H8 completo como unidad, NO la copia mezclada H7/H8 que permanece en Google Drive si los reemplazos fueron bloqueados. **Esta entrega es acumulativa**: incluye H8 íntegro más los cambios H9. Esta documentación no es un certificado de compilación ni de ensayo físico.

## Objetivo del sprint

Mejorar el firmware real sin alterar el contrato RTP de Michi Link, las credenciales de Home, las operaciones de Ecosystem ni Micro Server: mantener el audio continuo, preservar el mute y la rampa, fallar de modo observable si el DAC deja de consumir y permitir que el controlador diagnostique la causa. No afirmar artificialmente 78 % global por haber escrito código no probado.

## Fallos encontrados

- **H9-P0-01 (salida I2S aparentemente activa):** la tarea consumidora dequeuaba PCM y, si `i2s_channel_write()` fallaba, descartaba audio y seguía indefinidamente. Una avería persistente dejaba el motor aparentemente RUNNING. Se añade umbral de tres fallos consecutivos antes de cambiar a FAULTED y detener cooperativamente el consumidor. Los errores aislados no cierran la sesión.
- **H9-P1-02 (escritura incompleta):** `ESP_OK` con `written < submitted` no se consideraba fallo ni se reflejaba en métricas. Se detecta y contabiliza como una entrega PCM incompleta. No se reenvía ciegamente una cola parcial, que podría duplicarse en DMA.
- **H9-P1-03 (producer backpressure sin límite):** una llamada de `michi_audio_output_write` podía esperar para siempre cuando el ring dejaba de drenarse. Se añade presupuesto total de 1.500 ms en ticks bloqueados; al agotarlo se devuelve `ESP_ERR_TIMEOUT` y el motor de sesión existente corta de modo seguro. La política no afecta el retorno normal cuando hay espacio libre.
- **H9-P1-04 (shutdown tras fatal):** el consumidor limpiaba el estado a STOPPED y perdía la marca FAULTED. Se conserva la marca hasta un STOP explícito y se asegura la desactivación del canal I2S cuando el worker ya se había detenido. `deinit()` mantiene su regla de no destruir un canal con una tarea viva.
- **H9-P1-05 (observabilidad):** nuevos contadores atómicos bajo `s_err_lock` para `short_dma_writes`, `backpressure_timeouts` y `fatal_dma_streaks`; getter estructurado en `michi_audio_output.h`. El endpoint protegido `GET /api/v1/receiver-lite/diagnostics` incorpora `i2s_quality`, sin eliminar `i2s_errors`. `faulted` solo indica error de pipeline, NO medición del sonido analógico ni garantía de sincronía RTP.
- **H9-P1-06 (prueba H8 inconsistente):** había una aserción que exigía PCM cero inmediatamente después de mute, aunque H8 implementa un fade de 10 ms. La prueba se ajusta para esperar 480 fotogramas a 48 kHz. Se incorpora también caso de retarget en medio de la rampa y simulación de fallo DMA reiterado.
- **H9-P1-07 (mock incompleto):** el shim host de volumen incorpora los símbolos explícitos de mute para conservar la paridad de interfaces con H8 sin alterar el firmware real.

## Archivos de la entrega autocontenida

- `firmware/components/michi_volume/michi_volume.c` — H8 rampa completa
- `firmware/components/michi_volume/include/michi_volume.h` — API rampa
- `firmware/components/michi_audio_output/michi_audio_output.c` — H8 integridad + H9 fallos persistentes, backpressure, diagnósticos
- `firmware/components/michi_audio_output/include/michi_audio_output.h` — nueva API de diagnósticos H9
- `firmware/components/michi_http/http_server.c` — bloque opcional `i2s_quality` en diagnósticos autenticados
- `tests/host/shim/michi_volume_fake.c` — paridad de stubs
- `tests/host/test_michi_volume.c` — rampas, S16, S24, mute, retarget
- `tests/host/test_michi_audio_output.c` — frame alignment + fault policy y teardown
- `docs/H9_KILLCRITIC_FIRMWARE_RELIABILITY.md` — informe
- `docs/OPENCODE_H9_LOCAL_VALIDATION.md` — validación local

## Criterios de cierre (no cumplidos por escribir código)

1. OpenCode debe comparar el árbol completo con `.git`, hacer revisión de símbolos, compilar **host** sin warnings y ejecutar `make -C tests/host test` con resultados completos. **No ejecutar tests solamente sobre la parte H9:** H8 y H7 son dependencias.
2. Compilar firmware ESP-IDF para el perfil real; probar `test_michi_audio_output` con error I2S aislado, tres errores persistentes, caso de escritura corta y caso de ring que no drena. Si falta capacidad de inyección en el shim, añadirla antes de reclamar PASS.
3. Validar que `GET /api/v1/receiver-lite/diagnostics` conserva autenticación por Bearer y el campo `i2s_errors`; nuevas claves nunca contienen tokens ni secretos.
4. Verificar STOP/START/QUIESCE/RESUME tras FAULTED, sin bloquear el cierre ni liberar recursos usados por tareas vivas.
5. Ensayar en el ESP32-S3 con PCM5122 y PCM5102A (si están disponibles): inicio/parada, mute/unmute sin clic, latencia estable, pérdida Wi-Fi, CPU, PSRAM, watchdog, 24h. Un PASS host no sustituye hardware E2E.

## Riesgos deliberadamente fuera de H9

- H8/9 no corrigen los mensajes RTP por encima del MTU común ni la negociación de paquetes; requieren coordinación con Micro Server y Michi Link.
- Ninguna ampliación del formato de identidad/Home ni modificación OTA: Ecosystem continúa su trabajo en otro flujo.
- No se asegura que la atenuación física del DAC siga el volumen software después de que falle la escritura de volumen hardware; es un caso a medir.
- El corte abrupto por fatal I2S prevalece sobre minimizar clics: no debe dejar una sesión falsa por intentar silenciar delicadamente una salida que ya no escribe.

## Estimación de avance y objetivo

Referencia anterior: 68 % de madurez global estimada. **El objetivo 78 % es una puerta de aceptación, no un número atribuible a H9 sin evidencia.** H8+H9 mejoran el contenido de código y la cobertura diseñada, pero no resuelven la incorporación Home E2E, el audio analógico probado ni OTA. Nueva estimación prudente de código preparado: 72–74 % si el parche se integra completo; estado certificado global: no demostrable hasta los resultados de OpenCode y hardware. Para sostener 78 %: host + CI + compilación + E2E LAN reales + pruebas de errores críticos cerradas, con los problemas de incorporación enumerados explícitamente.

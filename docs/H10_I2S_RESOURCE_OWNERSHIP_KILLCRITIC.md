# Michi Music Stream — H10 KILLCRITIC: ownership, rollback y recuperación I2S

**Base:** estado H9 consolidado en la carpeta original de Drive (19-10-2026). **Alcance:** solo Michi Music Stream. **Evidencia:** revisión estática y código redactado; no se han ejecutado compilaciones, host tests, ESP-IDF ni ensayos físicos.

## Por qué H10 es necesario

1. **H10-P0-01 — STOP informa éxito tras fallo de `i2s_channel_disable`.** La rama normal de `michi_audio_output_stop()` detectaba el fallo pero continuaba hasta `MICHI_AUDIO_STATE_STOPPED` y devolvía `ESP_OK`. Podía autorizar una nueva sesión sobre un canal no deshabilitado. En la rama de worker terminado por fallo DMA sucedía algo equivalente.
2. **H10-P0-02 — DEINIT libera recursos tras fallo de `i2s_del_channel`.** La implementación anterior registraba el error, anulaba `s_tx`, liberaba el buffer PSRAM y destruía los semáforos. El driver podía seguir reteniendo el canal, sin posibilidad de recuperación limpia.
3. **H10-P1-03 — Rollback incompleto en error de creación de tarea.** Tras `i2s_channel_enable` correcto, si `xTaskCreate` fallaba, el `disable` de rollback se ignoraba. Un fallo encadenado podía dejar un canal habilitado y estado `INITIALIZED` (incorrectamente seguro para reintento).

## Cambios implementados

- **STOP normal:** espera el fin cooperativo del worker. Si `disable` devolvió error, conserva un estado `FAULTED` y los recursos; devuelve el error al llamador y bloquea `START`/`DEINIT`. Un nuevo `STOP` puede reintentar la deshabilitación del canal; solo un éxito publica `STOPPED`.
- **STOP después de fallo DMA:** no elimina `s_task_done` ni declara `STOPPED` si el `disable` falla; retorna el error y conserva la opción de reintento.
- **DEINIT:** cuando falla `i2s_del_channel`, retorna el error sin liberar el ring, los semáforos ni la propiedad `s_tx`. El flag interno `s_channel_delete_failed` impide `START` durante una propiedad de canal incierta. Un `DEINIT` posterior vuelve a intentar la operación; solo al completarla limpia los recursos y el flag.
- **START / task creation rollback:** si tampoco puede deshabilitar el canal al fallar la creación del worker, mantiene `FAULTED` recuperable mediante `STOP`, en lugar de devolver el sistema a `INITIALIZED`.
- **Host shim:** inyección de errores *one-shot* para deshabilitación/destrucción de I2S y lectura de estado de canal simulado. Nada de esto se compila en producción.
- **Pruebas de regresión nuevas (aún sin ejecutar):** fallos y reintentos de STOP, fallo en destrucción con preservación de recursos, rollback de START en fallo encadenado; preservación del comportamiento sin errores.

## Archivos afectados

1. `firmware/components/michi_audio_output/michi_audio_output.c`
2. `firmware/components/michi_audio_output/include/michi_audio_output.h`
3. `tests/host/shim/i2s_shim.c`
4. `tests/host/shim/driver/i2s_std.h`
5. `tests/host/test_michi_audio_output.c`

## Contratos intocables

Sin cambios en RTP v1-lite (48 kHz S16LE estéreo, 10 ms, payload de 1920 B, PT97), Michi Link, MICHI-F1, claves Home, descubrimiento, OTA, volumen/mute H8 o semántica de otros repositorios. No se amplían capacidades públicas no certificadas.

## Política de validación y criterios de aceptación

OpenCode debe usar `docs/OPENCODE_H10_LOCAL_VALIDATION.md`, descargar el árbol completo, revisar diff, compilar host + ESP-IDF y ejecutar las suites de regresión **en local**. Comprobar (a) que los nuevos fallos se propagan; (b) que jamás se publica `STOPPED` tras `disable` fallido; (c) que nunca se libera buffer/semáforos después de `del_channel` fallido; (d) que reintentos con driver recuperado funcionan; (e) que los host shims conservan compatibilidad con tests H1–H9.

El siguiente paso de alto impacto depende de hardware: medir la recuperación con Wi-Fi inestable y fallos intermitentes de DMA; asegurar que Micro Server distinga sesión activa, salida `FAULTED` y audio físicamente audible. No prometer 78 % con evidencia puramente estática.

## Reversión

Revertir los cinco archivos como un conjunto si los tests descubren una regresión. Preservar `.git` local. No eliminar estado Home ni usar un nuevo formato de provisión para probar H10.

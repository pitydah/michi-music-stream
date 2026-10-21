# Michi Music Stream — H7 KILLCRITIC: control de volumen y orden de sesiones

**Fecha:** 2026-10-19. **Base:** copia original de Google Drive posterior a H6. **Evidencia:** revisión estática y archivos modificados; **sin compilación, tests ejecutados ni certificación física**. Ecosystem y Michi Link se mantienen fuera de alcance.

## Hallazgos y correcciones

1. **H7-01 — Mute destructivo.** `michi_audio.c::output_ops_mute(true)` llamaba a `michi_volume_set(0)` y `mute(false)` no recuperaba el nivel. `michi_volume_set_muted()` mantiene mute ortogonal al volumen. Aplicación digital de silencio antes de I2S tanto en hardware-volume como en software-volume. `michi_volume_get()` indica nivel configurado, no el estado de mute; `michi_volume_is_muted()` indica el mute. El cambio de volumen mientras está muteado **no desmutea**; el valor nuevo se respeta al quitar mute. `michi_volume_init()` reinicia mute al inicio, sin usar NVS. Sin nuevos endpoints.
2. **H7-02 — Estado inter-core sin sincronización C.** Los `volatile` originales para volumen y hardware-path se leían desde la tarea I2S y se escribían desde la tarea de control en distintos núcleos: `volatile` no es una primitiva de sincronización C. H7 cambia los tres flags/valores compartidos a `_Atomic` C11. Las lecturas se capturan al inicio de cada chunk. **Límite:** esta política es efectiva desde el siguiente chunk; no es mute sample-accurate ni garantiza rampa antipop.
3. **H7-03 — Carrera de sesión al notificar.** `michi_session_patch()` leía `s_session.info` tras liberar el mutex; otra sesión podía cerrarse o reemplazarse. H7 publica eventos de pausa/reanudación bajo el mutex y realiza logs con un snapshot del volumen copiado dentro de la región crítica, evitando eventos de PATCH reordenados respecto a STOP/START.
4. **H7-04 — Tests.** Nueva suite `tests/host/test_michi_volume.c` enlazada con `michi_volume.c` **real**, DAC stub solamente. Casos: mute/unmute conserva el nivel; cambio de volumen mientras muteado; ruta DAC hardware; fallback digital; amplitudes PCM; clamp; entradas defensivas. Registrada en `tests/host/Makefile` y ejecutada por `make test` local.

## Archivos fuente

- `firmware/components/michi_audio/michi_audio.c`
- `firmware/components/michi_volume/michi_volume.c`
- `firmware/components/michi_volume/include/michi_volume.h`
- `firmware/components/michi_session/michi_session.c`
- `tests/host/Makefile`
- `tests/host/test_michi_volume.c` (nuevo)

## Compatibilidad y límites

- `michi_volume_get()` continúa proporcionando el nivel **configurado** que utiliza la API de volumen; si mute=true el PCM efectivo es cero. Cualquier cliente que confundiera nivel y silencio deberá leer la señal mute explícita, no inferirla del nivel.
- No se modificó la negociación v1-lite: RTP/UDP PT97, 48kHz/16-bit/2ch/10ms/1920 bytes, descubrimiento, Home membership, metadatos, partición de fábrica y OTA siguen iguales.
- Al cambiar a un DAC que informa volumen hardware y cuyo write falla, la ruta existente cae a digital. **No se demuestra físicamente el nivel efectivo del DAC** y podría persistir una atenuación hardware; todavía requiere pruebas y una política segura de error DAC.
- La puerta de PCM mute requiere evaluación acústica de clicks/pops, transitorios y comportamiento al recibir escrituras simultáneas; no sustituye un fade/ramp.
- Se deben revisar los tests existentes de `michi_audio_output` y `michi_session`, así como el estado del DAC tras PAUSE y nueva sesión.

## Validación requerida

Seguir `docs/OPENCODE_H7_LOCAL_VALIDATION.md`. No declarar H7 PASS sin la suite host, compilación ESP-IDF y E2E físico. Si hay regresión, revertir las seis rutas fuente/tests de forma conjunta.

## Pendientes P0/P1 fuera del sprint

1. Productor de metadatos y estados correlacionados desde Micro Server y WebUI.
2. Contract/bootstrap Home de Ecosystem + Michi Link aún en implementación externa.
3. Fragmentación IP del RTP de 1.920 bytes payload sobre Wi-Fi/MTU 1.500, jitter de red y métricas E2E.
4. Certificación hardware de DAC PCM5122/PCM5102A, mute/unmute sin clic y 24h de reproducción.
5. OTA A/B, rollback por interrupciones, protección de credenciales y recuperación USB sin botón.
6. Pruebas de concurrencia con cambios rápidos de volumen/mute, STOP/START, PAUSE/RESUME; métricas de latencia y pérdida.

## Madurez estimada

Ver respuesta final. Los porcentajes corresponden a estimaciones de preparación; **la madurez global no puede considerarse certificada** sin las pruebas requeridas.

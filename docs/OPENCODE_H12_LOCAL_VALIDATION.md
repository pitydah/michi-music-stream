# OpenCode / Antigravity — Validación LOCAL H12 (SIN saltar gates)

Actúa como ingeniero ESP-IDF/FreeRTOS senior y auditor KILLCRITIC de Michi Music Stream. Trabaja SOLO sobre repositorio local donde el dueño descargó Drive y superpuso `MICHI_MUSIC_STREAM_H12_PRETEST_CONSOLIDATED.zip`. **No elimines ni reemplaces `.git`**, NVS, secretos ni archivos ajenos. Reporta HEAD antes/después.

## Puerta 0: integridad

- Usa `docs/H12_SOURCE_MANIFEST.json`: comprueba SHA-256 para todas las rutas declaradas. Investiga cualquier drift.
- Revisa `git status`, `git diff --check`, `git diff --stat`, cambios H8–H12 completos y duplicados en paths.
- Detecta dependencias ausentes, macros ESP-IDF y símbolos mock inexistentes; NO sustituyas fuente productiva por shim de host.

## Puerta 1: host

- Compila con advertencias estrictas y ejecuta la suite completa `make -C tests/host test` y después `./scripts/run_tests.sh --full` con dependencias instaladas. No omitas suites en silencio.
- Revisa nuevas regresiones H11/H12: INIT delete fail, start/deinit race, writer activo, stop timeout/disable, quiesce short DMA `ESP_OK` pero `written < submitted`, barrera PCM no reabierta tras STOP/FAULTED.
- Comprueba SPSC, orden de locks (state→ring), ausencia de deadlocks, seguridad de semáforos FreeRTOS y recuperación tras errores encadenados.

## Puerta 2: ESP-IDF

- Compila en `firmware/` con la versión IDF configurada y **perfil ESP32-S3 real**, limpia cachés SOLO si es seguro; no falsees PASS.
- Audita `i2s_new_channel/init_std_mode/enable/disable/del_channel` en rutas de error, `xTaskCreate` y task self-delete, cambios ISR/FreeRTOS y `s_notify_inflight`.
- Revisa integridad de PCM S16/S24, streaming RTP, overrun del jitter buffer, `mute` y rampas H8, observabilidad H9 y falla segura H10–H12.

## Puerta 3: integración/seguridad

- Verifica contratos v1-lite HOME_MEMBERSHIP y no regresión de claves/OTA. Comprueba que Micro Server y Stream utilizan la misma geometría RTP (payload 1920B, PT 97) y registra explícitamente riesgo de fragmentación IP/MTU.
- Ejercita con host/simulador sesión AUTH, descubrimiento mDNS/Whisker, heartbeat, STOP/START/PAUSE/RESUME y error de canal. Asegura no exposición de secretos por diagnostics.
- No declarar interoperabilidad real con Micro Server ni DAC sin medición física.

## Puerta 4: ensayo físico (solo con autorización expresa del dueño)

- Preparar checklist de flasheo seguro, backups de NVS, cables y pines PCM5122, UART, logs y rollback; NO flashear automáticamente ni borrar identidad Home.
- Test Wi-Fi conectado y desconectado, timestamp, anuncios, mDNS, challenge/session, RTP/UDP real, auriculares/RCA, mute sin clic, fallo DMA, 24h.
- Medir continuidad, jitter, buffer underruns, temperatura, heap/PSRAM, CPU, watchdog, reconexión, latencia y comportamiento de pérdida/reordenación/fragmentación.

## Resultado exigido

Genera informe PASS/FAIL/BLOCKED **por puerta**, logs completos, SHA de commit, tablas de riesgos P0/P1/P2 y correcciones. Si algo falla: arregla en local de manera mínima, vuelve a ejecutar gates; jamás afirmes 85/100% de funcionamiento sin evidencia. No edites Michi Link ni Micro Server sin instrucción explícita.

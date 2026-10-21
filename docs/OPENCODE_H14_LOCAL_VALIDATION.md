# OpenCode / Antigravity — validación estricta Michi Music Stream H14

Actúa como firmware engineer ESP32-S3 + ESP-IDF v5.3 y auditor KILLCRITIC. Esta entrega se preparó en Google Drive **sin ejecutar pruebas**. Debes realizar validación y corrección del árbol completo, no limitarte a redactar un informe. El usuario descargó la carpeta de Drive y extrajo `docs/MICHI_MUSIC_STREAM_H14_PRETEST_CONSOLIDATED.zip` en su raíz conservando `.git`.

## Fase 0: integridad y baseline

Inspecciona `git status`, `git diff --check`, hashes en `docs/H14_SOURCE_MANIFEST.json`, `idf.py --version` y el contrato Michi Link. Confirma que los seis archivos fuente/test/shim H14 están realmente incorporados. No hagas push sin autorización. No borres identidad, NVS ni realices flasheo destructivo.

## Fase 1: compilación y suite completa

Ejecuta `make -C tests/host clean && make -C tests/host test`, `./scripts/run_tests.sh --full`, CI equivalente e `idf.py build` dentro de `firmware`. No ocultes errores ni omitas suites. Revisa warnings tratados como errores, `_Atomic(TaskHandle_t)`, `xTaskGetTickCount` real y en shim, `ulTaskNotifyTake` y compatibilidad ESP-IDF v5.3. Si falla, modifica fuente/pruebas y vuelve a ejecutar todas las puertas.

## Fase 2: probar realmente el motor RTP (obligatorio)

`test_michi_session.c` utiliza `michi_audio_fake` y NO demuestra las garantías del motor RTP real. Crea o amplia un harness que compile `michi_audio.c` real con shims de red FreeRTOS para inyectar fallos y ordenar intercalaciones determinísticamente. Cubrir:
- `xTaskCreate` exitoso pero el worker se agenda inmediatamente; demostrar que no sale antes de publicación y notificación, y que no se notifica un handle obsoleto.
- creación de tarea fallida, fallo bind(), fallo allocation PSRAM, fallo I2S STOP y START: sin leak, socket abierto, estado activo fantasma ni métricas de una sesión no iniciada.
- STOP durante startup y prefilling; autoconclusión por fallo UDP y `start` posterior; RUNNING/FAULTED tras QUIESCE fallido; señales C11 y ausencia de deadlock.
- pausa/resume bajo recepción continua, underrun, jitter y cierre del motor; recuperación sin reinicio del ESP32-S3 si es legítima.

## Fase 3: motor I2S PCM

Probar el nuevo `test_h14_total_pcm_write_deadline`, con tiempos tolerantes y pruebas host reproducibles. Comprobar ticks wrap-aware, comportamiento con `CONFIG_FREERTOS_HZ=100` y 1000, watchdog, DMA lento, ring lleno con progreso intermitente, STOP/QUIESCE/DEINIT y cuelgues. Mantener la regla de un solo escritor I2S y verificar rutas H11–H13.

## Fase 4: integración y veracidad de diagnóstico

Correlacionar IP, SSRC, home_id, identidad, anuncio firmado Whisker/mDNS, autenticación, socket RTP, jitter, ring, DAC con Micro Server y su WebUI. Si Micro Server requiere cambios para interpretar estados, desarrollar también la WebUI en ese repositorio; no cambiar contrato unilateralmente. Verificar fragmentación 1920B/MTU, habilitación de IPv4 reassembly, pérdidas, disrupciones Wi-Fi, seguridad de red y OTA con rollback.

## Fase 5: físico, solo con autorización y equipo

Flashear con mecanismo autorizado, verificar ESP32-S3 + PCM5122, clocks y DAC, mute/gain, pausas sin clics, conexiones repetidas, 24h, RAM/PSRAM y watchdog. Prueba segura de recuperación USB sin botón, corte de energía OTA y preservación Home.

## Entrega obligatoria

Informe con HEAD exacto, SHA del build, test/CI runs, G0–G7 PASS/FAIL/BLOCKED, logs de todos los fallos, memoria heap, posibles P0/P1/P2, diffs, explicación por cada corrección. **Nunca marcar PASS si no fue ejecutada; nunca declarar 100% certificado sin prueba física.**

# MICHI MUSIC STREAM — H13 KILLCRITIC: cierre de concurrencia y recuperación de sesión

**Fecha:** 2026-10-19. **Base:** H12 consolidado + fuente vigente de `michi_audio.c` en Google Drive. **Alcance:** código preparado para validación local. **NO compilado / NO ejecutado / NO flasheado**.

## Motivo

La auditoría posterior a H12 encontró: (1) UAF potencial de semáforos en QUIESCE/RESUME cruzado con STOP y DEINIT; (2) flags `volatile` entre tareas en el motor RTP sin sincronización C11; (3) START/STOP/Pause sin lease de transición de sesión; (4) stop RTP descartaba errores del silencio; (5) falta de reconciliación de FAULTED al abrir una nueva sesión; (6) tarea RTP terminando antes de publicación de su handle.

## Cambios reales H13

- **I2S** (`michi_audio_output.c`): `s_control_inflight` protegido por `s_state_lock`. Una solicitud admitida conserva referencia de vida útil desde que valida estado hasta liberar el mutex/ACK. DEINIT rechaza liberar semáforos si queda referencia viva, incluso tras STOP. La transición de INIT reinicia contador. Hook host solo para intercalación determinista.
- **Host I2S** (`test_michi_audio_output.c`): test `test_h13_control_reference_blocks_deinit()` bloquea un comando después de liberar su mutex pero antes de soltar la referencia; STOP debe completar y DEINIT debe rechazar mientras el comando siga vivo, y admitirlo después. Este test se incluye en `main`. No se ha ejecutado.
- **RTP** (`michi_audio.c`): reemplazo de seis flags/handle `volatile` por `_Atomic` C11, y lease estático fail-closed para START, STOP y PAUSE. Ningún spinlock se mantiene mientras se crean tareas, se reciben sockets o se espera I2S.
- **RTP startup**: evita publicar un task handle obsoleto si el worker murió inmediatamente; consume un done flag anterior antes de comenzar nueva generación.
- **RTP shutdown**: conserva errores de QUIESCE; distingue sesión de red terminada y salida `FAULTED`. El inicio siguiente permite que STOP reconcilie `FAULTED`/`STOPPING` antes de abrir una nueva sesión, y rechaza si la recuperación no funciona.

## Invariantes y contratos preservados

- RTP: PCM S16LE 48 kHz, estéreo, 10 ms, 1.920 bytes, PT=97, validación negociada de SSRC y origen.
- Home membership, claves, certificados, NVS, BLE, Wi-Fi, descubrimiento Whisker/mDNS, APIs Michi Link y particiones/OTA: **sin cambios**.
- Arquitectura de un solo productor RTP/un consumidor I2S; solo audio, ninguna funcionalidad de biblioteca/playlist.
- No se anuncia `now_playing` como funcional ni se proclama certificación DAC.

## Riesgos no resueltos ni atribuibles a H13

1. IPv4 fragmentación: `CONFIG_LWIP_IP4_REASSEMBLY=y` habilita reensamblado, pero el payload RTP de 1.920 B excede la MTU habitual. Es obligatorio medir redes reales y acordar cualquier cambio de geometría en Stream/Micro Server/Michi Link simultáneamente.
2. E2E real con Micro Server y su WebUI (mDNS, Whisker firmado, Home, auth/session, audio PCM).
3. OTA A/B/rollback, interrupciones eléctricas, recuperación USB autorizada sin botón y protección de credenciales.
4. Certificación ESP32-S3/PCM5122: clocks, pops/mute, muestras DMA, latencia, PSRAM, watchdog, escucha de 24 h.
5. `volatile` en otros componentes del repositorio: no se declaran auditados exhaustivamente por H13.
6. Los shims no representan necesariamente la planificación FreeRTOS ni latencias reales del DMA.

## Entrega y verificación

- `MICHI_MUSIC_STREAM_H13_PRETEST_CONSOLIDATED.zip`: parche acumulativo de fuentes H8–H13 con rutas relativas, **NO** el repositorio completo.
- El repositorio fuente real está en la carpeta original de Drive, con los cuatro archivos principales H13 actualizados. Si hay conflicto de copia o edición simultánea, preferir los SHA-256 del manifiesto H13.
- `H13_SOURCE_MANIFEST.json`: checksum de cada archivo incluido; `H12_SOURCE_MANIFEST.json` se conserva solamente como evidencia histórica H12.
- No declarar PASS ni 100% funcional hasta ejecutar `OPENCODE_H13_LOCAL_VALIDATION.md` y las pruebas físicas. El código H13 queda preparado, no certificado.

## Nota de honestidad

La cifra "100%" aplica como máximo a la integridad y cierre del conjunto de cambios H13 **diseñados**. La madurez global del producto, interoperabilidad y calidad del audio físico aún requieren evidencia. Si la compilación muestra problemas de ABI/headers o el nuevo test falla, H13 permanece abierto para OpenCode/Antigravity.

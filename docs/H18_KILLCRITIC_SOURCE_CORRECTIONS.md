# Michi Music Stream — H18: RTP real host harness + snapshot fail-closed

**Baseline de firmware:** H16R-PRETEST sobre H15. **Fecha:** 2026-10-19. **Alcance:** solo ingeniería fuente desde Google Drive, sin builds ni tests ejecutados.

## A. Qué se modifica y por qué

El diagnóstico H17 identificó dos carencias de preparación de fuente dentro del 100% Drive:

1. `test_michi_session.c` enlaza `michi_session.c` real, pero **NO** `michi_audio.c` real; el motor RTP estaba probado únicamente a través de guard, jitter y pause_policy independientes. No existía una prueba host que recorriera el arranque UDP real del motor y la carrera PAUSE/recvfrom.
2. El generador de snapshots H17 podía producir un ZIP íntegro **pero incompleto** si faltaba un directorio crítico tras descargar Drive. SHA-256 certifica contenido presente, no contenido ausente.

## B. H18 nuevo objetivo test_michi_audio_engine

`tests/host/Makefile` añade el objetivo al `TESTS`, al ciclo `make test` y a `make clean`. **El objetivo enlaza exactamente las fuentes de producción**:

- `firmware/components/michi_audio/michi_audio.c` (motor RTP)
- `firmware/components/michi_audio/michi_jb.c` (jitter real)
- `firmware/components/michi_audio/rtp_guard.c` (validador real)
- `firmware/components/michi_audio/michi_audio_buffer.c` (validación de buffering)

Usa `tests/host/shim_rtp/` para sockets UDP POSIX sobre loopback, tiempo monotónico del host, FreeRTOS-pthreads ya existente, doble de salida I2S/DAC y fault injection de asignación y bind. Los shims están aislados por include path específico del objetivo; no se insertan en firmware ni sustituyen la implementación RTP de producción.

### Casos redactados

1. Fallo al reservar contexto -> `ESP_ERR_NO_MEM`, sin sesión fantasma.
2. Fallo de `bind()` inyectado -> error, socket cerrado y sin sesión.
3. Fallo de `xTaskCreate` -> rollback y ausencia de sesión.
4. Creación UDP y anuncio de puerto elegido 49152..65535.
5. RTP verdadero S16LE 48k/16/2 PT97, 1.920B PCM, SSRC negociado -> contadores y entrega al fake I2S.
6. RTP con SSRC diferente -> contador de rechazos.
7. PAUSE después de arranque RTP y durante espera UDP; el worker continúa vivo.
8. RESUME y recepción de nuevos paquetes.
9. STOP, cierre cooperativo y nuevo START del motor real.
10. Fallo de QUIESCE al hacer STOP -> error propagado, no sesión fantasma y recuperación en el siguiente START.
11. Sin `vTaskDelete(task)` externo.

**Puertas no cubiertas aún**: intercalación determinista exacta en cada ciclo de `recvfrom`/PCM, interrupciones reales de I2S, errores DMA, fugas de memoria certificadas por ASan/Valgrind, hardware PCM5122; estos requieren ampliar el harness o tests locales después. La prueba es de integración HOST, NO certificación de Wi-Fi/I2S/DAC físico.

## C. Protección de snapshots H18

`package_source_snapshot.py` ahora invoca `validate_required_sources` y rechaza una descarga incompleta aun cuando el ZIP y su SHA sean internamente consistentes. Comprueba una lista explícita de carpetas (incluidos contratos Michi Link) y archivos obligatorios: los motores RTP, jitter, I2S, sesión, HTTP, app_main, Build/Makefile, runner y todas las fuentes del harness RTP real. Rechaza files ausentes y files vacíos. Conserva exclusión de caches, `.git`, builds, binarios y secretos evidentes, así como publicación atómica y hashes de cada archivo.

`test_source_snapshot.py` actualiza el proyecto de prueba sintético y añade fallos deliberados por motor RTP eliminado o contratos ausentes. No se ha ejecutado pytest.

## D. Preservación deliberada

- No se cambia contrato RTP (PT97, S16LE, 48kHz, 10ms/1920B), IP/SSRC, Michi Link/Home, BLE, aprovisionamiento sin botón, Wi-Fi, OTA o NVS.
- **No se modificó** `michi_audio.c` de producción: H16R ya conservaba las correcciones de PAUSE y recuperación. Reescribirlo sin evidencia sería regresión.
- No se sustituye `michi_session.c` H16R (contiene watchdog autónomo). El objetivo H18 verifica el motor que H16R controla.
- El baseline técnico del firmware continúa **H16R-PRETEST**; H18 añade harness y paquete, no nueva versión firmware certificada.

## E. Entrega/estado

Archivos fuente/tests redactados y añadidos a Google Drive, hashes y paquete overlay H18 reproducible para inspección. **NO se ejecutó** `make`, pytest, CI ni `idf.py`; el test nuevo está pendiente de compilación y depuración en OpenCode/Antigravity.

No afirmar 100% de ingeniería de Drive todavía: quedan documentación diagnóstica de BUFFERING vs PLAYING, hardening final de Home/Wi-Fi/OTA y snapshot integral validado desde la carpeta completa. Un nuevo fallo de compilación del harness debe corregirse antes de declarar el gate como aprobado.

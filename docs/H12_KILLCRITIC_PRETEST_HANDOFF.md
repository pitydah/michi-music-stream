# MICHI MUSIC STREAM — H12 Consolidación previa a tests físicos

Fecha: 2026-10-19. Estado: **fuente redactada y consolidada; no compilada, no ejecutada, no flasheada**.

## Propósito

H12 cierra dos P0 adicionales detectados al revisar el ciclo de vida de H11. El ZIP es **acumulativo** sobre la carpeta original: contiene el conjunto H8/H9 íntegro (volumen, HTTP, tests), todos los cambios H10/H11 del módulo I2S y H12 con el mismo árbol de rutas. No contiene el repositorio completo: se debe descargar la carpeta original de Google Drive y superponer este ZIP (conservar `.git` del repositorio local), NO sustituir el repositorio por el ZIP.

## Código corregido (H12)

1. `michi_audio_output_flush()` toma una referencia `s_write_inflight` después de validar el estado bajo `s_state_lock`, ejecuta el flush bajo `s_ring_lock` y libera la referencia al terminar. START y DEINIT ya verifican ese contador (H11): se evita liberar o reutilizar el ring con una operación admitida en curso. Se rechaza FLUSH si una barrera de quiesce está activa.
2. QUIESCE ya no considera éxito `ESP_OK` con `written < sizeof(s_chunk)`: se marca `FAULTED`, retorna error e incrementa contadores. Ni silencio parcial ni write fallido se anuncian como `QUIESCED`.
3. Un QUIESCE fallido o rechazado ya no reabre incondicionalmente la barrera PCM cuando una transición STOP/FAULTED ha asumido el control. La decisión queda serializada por `s_state_lock` y `s_ring_lock` en orden estado→ring, evitando invitar escrituras tardías.
4. Host shim reproduce una escritura I2S parcial con `test_i2s_set_next_write_short()`, solo en host. Se incorpora el caso de regresión del silencio incompleto en quiesce y la recuperación STOP/DEINIT posterior.

## Cambios de H11 heredados

- Lease no bloqueante entre INIT, START, STOP y DEINIT para serializar propiedad del handle I2S.
- `s_write_inflight` para impedir que un productor PCM activo sea liberado por DEINIT o reutilizado por START.
- STOPPED solo después del disable exitoso, reintentos seguros tras fallo de disable/delete, cuarentena de INIT parcial.
- Shims de fallos y regresiones intercaladas de ciclo de vida.

## Límite de alcance

No modifica la negociación RTP PT97 / PCM S16LE 48 kHz / estéreo / 10 ms / 1920 bytes, ni el formato de identidad Home ni claves, NVS, la provisión USB, Michi Link, Micro Server, Ecosystem o OTA. La fragmentación de un datagrama UDP de este tamaño en redes con MTU típica 1500 **sigue pendiente P0** y exige evolución de contrato simultánea con Micro Server. No confundir este ZIP con una imagen ESP32-S3 flasheable o con una revisión probada.

## Evidencia y semáforo

- Archivos de fuente de H8/H9 y H10/H11/H12: PREPARADOS, sujetos a comprobación de integridad.
- Compilación host, tests C, ESP-IDF build, ABI, concurrencia real, watchdog: **NO EJECUTADOS**.
- Incorporación Home, descubrimiento Whisker/mDNS, Micro Server y DAC PCM5122 físico: **NO VERIFICADOS**.
- Pruebas de OTA A/B/rollback, pérdida Wi-Fi e interoperabilidad RTP MTU: **PENDIENTES**.
- Cumplimiento 100%: **100% del paquete documental y de fuentes H8–H12 preparado para validación, NO 100% de funcionamiento certificado.**

## Integración segura

1. Hacer backup y preservar `.git` y cambios locales.
2. Descargar la carpeta `michi-music-stream` de Drive.
3. Extraer el contenido de este ZIP sobre la raíz de ese árbol (sus rutas son relativas a la raíz).
4. Comparar `sha256sum` de las rutas incluidas con `docs/H12_SOURCE_MANIFEST.json`. Ejecutar diff `git status`, `git diff --check`.
5. Usar `docs/OPENCODE_H12_LOCAL_VALIDATION.md` para que OpenCode/Antigravity compile, corra suites y corrija defectos.
6. SOLO luego de PASS host/ESP-IDF y revisión de seguridad, ejecutar pruebas en ESP32-S3 bajo supervisión del propietario.

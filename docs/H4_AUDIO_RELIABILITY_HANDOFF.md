# Michi Music Stream — H4: RTP/Jitter Buffer Reliability & Memory

**Estado:** fuentes modificados y pendientes de compilación, tests y hardware en OpenCode. **No certificado.**
**Alcance:** solo Michi Music Stream, independiente del trabajo I1–I5 de Michi Ecosystem. No editar bundle Michi Link, MICHI-F1/Home, API HTTP, OTA ni Micro Server en esta pasada.

## Diagnóstico KILLCRITIC

1. `michi_audio.c` asignaba un slot en PSRAM por cada paquete del jitter buffer con el tamaño de recepción UDP `CONFIG_MICHI_AUDIO_RX_BUF_BYTES` (por defecto 8192 bytes), aunque `rtp_guard.c` admite únicamente `pcm_s16le` con payload de **1920 bytes**. Para 50 slots se reservaban 409600 bytes, frente a 96000 realmente necesarios. No se alteran los formatos RTP ni los milisegundos negociados.
2. `michi_jb_insert_packet()` truncaba una copia de payload a la capacidad del slot, pero conservaba `slot->len` sin truncar: con una ruta futura sin el guard estricto esto podría hacer que el consumidor leyera fuera del slot. Ahora rechaza **antes de evictar** el paquete si no cabe o si faltan los bytes en un pool de producción.
3. `michi_jb_oldest()` tiene una semántica especial documentada para políticas de eviction: si *todos* los paquetes están por detrás del `playhead`, devuelve el más cercano al playhead. `session_drain()` lo trataba como un futuro: la conversión de -1 a `uint16_t` produce 65535, equivalente a una cantidad extrema de silencio. Ahora purga paquetes obsoletos antes del playout con comparación wrap-aware.
4. El timeout de `SO_RCVTIMEO` no estaba comprobado. Si falla, un socket que bloqueara sin límite impediría que `stop()` reuniera a la tarea dentro de su plazo. Ahora el establecimiento de sesión falla antes de hacer bind y crear la tarea.
5. `Kconfig` describía un pool con stride UDP y una lógica fija de re-prefill que no correspondían al código actual; corregido.

## Cambios de archivos

- `firmware/components/michi_audio/michi_audio.c`: stride PCM separado del datagrama UDP; pool compacto, buffer de silencio compacto; descartar obsoletos, métrica `late` y fail-closed SO_RCVTIMEO.
- `firmware/components/michi_audio/michi_jb.c`: límites del payload, no recortar silenciosamente; nueva `michi_jb_discard_stale`.
- `firmware/components/michi_audio/include/michi_jb.h`: API para purga de obsoletos y aclaración del stride.
- `firmware/components/michi_audio/Kconfig`: información actual de memoria y underrun.
- `tests/host/test_michi_jb.c`: regresiones contra secuencias antiguas, wrap 65535→0, slots sobrescritos, datos PCM nulos, inicialización inválida.

## Memoria esperada, configuración por defecto

| Reserva | Antes | H4 | Ahorro |
|---|---:|---:|---:|
| PSRAM de audio en jitter, 50 slots | 409600 B (400 KiB) | 96000 B (93,75 KiB) | **313600 B (306,25 KiB; 76,56%)** |
| Buffer de silencio interno | 8192 B | 1920 B | 6272 B |
| Buffer RX UDP interno | 8192 B | 8192 B | 0 B |

La cantidad **es una derivación de tamaños de código**, no una lectura de memoria medida en placa. El ahorro PSRAM es adicional a estructuras, DMA y buffers TCP/IP, que no se tocan.

## Invariantes exigibles

- El motor sigue rechazando cualquier RTP que no sea PT 97, 1920 bytes PCM, SSRC negociado y fuente IPv4 válida.
- El jitter máximo, prefill y contrato RTP de 10 ms no cambian.
- Una copia PCM > 1920 B o sin payload no debe sobrescribir slots ni desalojar audio correcto.
- Los paquetes retrasados no deben generar pausas multi-segundo, ni retroceder playhead, ni contaminar el sonido.
- Cualquier error `setsockopt(...SO_RCVTIMEO...)` debe impedir abrir la sesión sin recursos retenidos.
- La preparación del DAC no debe ver cambios en códecs, I2S o Kconfig no relacionados.
- No afirmar mejor audio, 24 bits, 96 kHz o multiroom por esta optimización: la finalidad es **seguridad de memoria y robustez**, no cambiar fidelidad.

## Riesgos y pruebas exigidas en OpenCode

1. Compilar el test host de `michi_jb`, ejecutar con ASan/UBSan si son viables, y agregar los casos de buffer lleno antes de rechazo y secuencia wrap.
2. Compilar ESP-IDF `esp32s3` limpio con `-Werror` si la cadena lo admite; verificar que `michi_jb.h` exporta la nueva función en el componente.
3. Integrar 500, 1000 y 2000 ms de capacidad con una placa con PSRAM, verificando heap mínimo y memoria libre.
4. Hacer reproducción continua con pérdida, duplicados, reordenación y PAUSE/RESUME; comprobar que `late`, `lost`, `overruns`, `underruns` no se convierten en comportamientos incoherentes.
5. Cortar Wi-Fi y reiniciar Micro Server durante la reproducción; observar liberación de recursos, cierre de sesión y reconexión.
6. Verificar I2S/PCM5122 durante 24 h y comparar latencia y contadores de underrun con el baseline.
7. Si la compilación o los tests fallan, reparar en la copia local e informar archivos, errores y diff; **no marcar H4 como certificado** hasta ese momento.

## Fuera de alcance para no interferir con Ecosystem/OpenCode

- No modificar `michi-link` ni su bundle alpha.5.
- No cambiar `MICHI-F1`, Home identity ni la incorporación desde Ecosystem.
- No corregir MTU/RTP en un único extremo; cualquier cambio a la paquetización requiere un ADR y cambios coordinados en Micro Server.
- No iniciar trabajo OTA/WebUI ni el protocolo de metadata `now-playing` en este sprint.

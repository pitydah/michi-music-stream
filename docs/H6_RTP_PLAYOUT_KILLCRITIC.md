# Michi Music Stream — H6 RTP Playout / Jitter Buffer, KILLCRITIC

Fecha: 2026-10-19. Base: copia de Google Drive posterior a H4/H5, firmware ESP-IDF de Michi Music Stream.

## Estado y límites

**Implementación de código preparada. NO se ejecutaron compilaciones, pruebas host, CI ni hardware.** La compatibilidad efectiva debe verificarse en local. No se toca Michi Link, Michi Ecosystem, Micro Server, OTA, datos Home, red ni la geometría RTP (48 kHz, S16LE, dos canales, 10 ms, 1920 bytes de PCM, PT=97). `features.now_playing` continúa false.

## Hallazgos corregidos

1. **JB-OVERRUN/P0 lógico.** `michi_jb_insert_packet()` expulsaba el paquete **más cercano al playhead** para incluir uno futuro cuando el buffer estaba lleno. Esto podía dejar al consumidor sin el siguiente PCM, aumentando cortes y silencio. H6 conserva los paquetes inmediatos; elimina primero cualquier slot ya obsoleto y, si todos son futuros, admite el nuevo paquete solamente si está más cerca del cursor que el más lejano existente. De otro modo lo descarta sin mutar slots ni payloads. El límite del buffer se mantiene.
2. **JB-LATE/P1.** El buffer acepta ahora solo secuencias no anteriores al playhead, incluso si tuviera capacidad libre (distancia RTP de 16 bits con wrap). El clasificador de Stream ya filtra las tardías, pero reforzar la frontera de datos previene futuros usos incorrectos de la API interna.
3. **RTP-METRICS/P1.** `stream_policy()` incrementaba `provisionally_missing` antes de saber si `jb_insert()` había admitido realmente el datagrama. H6 actualiza high-water, reordering y provisional loss solo después de una inserción exitosa. Los drops/duplicados no alteran el avance de secuencia. Las métricas `overruns` continúan contabilizando saturación, no una reproducción comprobada.
4. **Diagnóstico.** Los logs de buffer lleno ahora distinguen slot descartado/reclamado en vez de afirmar que se expulsó el paquete más viejo.

## Archivos modificados

- `firmware/components/michi_audio/michi_jb.c`
- `firmware/components/michi_audio/include/michi_jb.h`
- `firmware/components/michi_audio/michi_audio.c`
- `tests/host/test_michi_jb.c`

## Casos de regresión incorporados (no ejecutados)

- Buffer completo 32/32: datagrama más lejano descartado y `seq=playhead` permanece intacto.
- Llegada reordenada y próxima: expulsión del futuro más lejano, preservación del PCM inmediato y verificación de los bytes del slot sustituido.
- Paquete tardío y duplicado: no cambian el conjunto válido ni expulsan audio.
- Salto `65535→0`: selección de cursor/futuros coherente y recuperación del slot obsoleto.
- Capacidad exacta: cuenta invariante, sin overrun que suprima PCM prioritario.
- Se preservan las regresiones H4/H5 anteriores de payload/heap/timeout UDP.

## Riesgos que siguen abiertos

- El playhead y la entrada de red comparten una tarea; una política mejor de overrun no elimina jitter de red real, ausencia de DMA ni fragmentación UDP/IP (datagrama RTP total ~1960 bytes, por encima de MTU Ethernet 1500 típica).
- `michi_jb_oldest()` mantiene el comportamiento histórico de buscar la entrada obsoleta menos lejana si *todas* son anteriores: la capa de playout llama `michi_jb_discard_stale()` antes de usarla. OpenCode debe comprobar la invariante de esa separación.
- El mute/unmute de output ops no restaura nivel previo; NO se cambió aquí para evitar mezclar semántica de volumen con un cambio de playout. Debe tratarse en una intervención que unifique adaptador de salida, michi_volume y session API.
- Los porcentajes de madurez son **estimaciones**, no certificados: no modificar avance global del 67 % hasta validar este sprint y revisar resultados.

## Entrega y reversión

El usuario descarga la copia actualizada de Drive (o el ZIP H6 si falla sincronización), aplica sobre su repositorio local preservando `.git`, ejecuta validación con OpenCode, y anota SHA/CI/placa/resultados antes de cerrar H6. Si falla una regresión, revertir las cuatro rutas de H6 como unidad. No escribir secretos ni resultados de simulación como pruebas físicas.

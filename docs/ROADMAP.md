# Michi Music Stream — Roadmap

## Fase 0: Contrato
Documentación, especificación v1-lite, ejemplos, tests.

## Fase 1: Discovery + Pairing
mDNS, botón físico, ventana 120 s, NVS tokens.

## Fase 2: Info + Heartbeat
Endpoints `/info`, `/firmware`, `/heartbeat` con timeout.

## Fase 3: Sesión + Volumen
`/session/start`, `/session/stop`, `/volume`.

## Fase 4: Audio PCM real
UDP rx, buffer circular, I2S DAC, pcm_s16le 48 kHz, Opus.

## Fase 5: Hi-Fi DAC
pcm_s24le, 96 kHz, MCLK, buffer 500 ms.

## Fase 6: OTA
Actualización de firmware Over-The-Air.

## Fase 7: Multiroom / Sync
Sincronización entre múltiples receptores.

## No planeado
Biblioteca musical, playlists, indexación, reproducción autónoma, Mobile como fuente directa.

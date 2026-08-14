# Michi Music Stream — Networking

## Conectividad

Wi-Fi Station. No opera como AP.

## Discovery

- mDNS como `_michi-link._tcp`, en el puerto HTTP real del dispositivo.
- TXT obligatorio: `device_id`, `service`, `api_version`, `roles` (string
  plano `audio_receiver`, nunca JSON), `michi_id`.
- UDP multicast firmado: grupo `224.0.0.167`, puerto `53318`, TTL 1,
  datagrama único ≤ 1200 bytes. Se anuncia al arrancar, al cambiar IP y cada
  30 s ± 3 s. El grupo firmado (`michi_id`, `public_key`, `nonce`,
  `timestamp`, `signature`) sigue los golden vectors de Michi Link.
- `device_id` es el mismo UUID que `server_id`.

## Transporte de audio

RTP/UDP hacia el puerto elegido por el receptor (49152–65535). Payload type
exacto `97`, PCM S16LE 48 kHz estéreo, 10 ms por paquete (1920 bytes). Se
valida versión RTP, PT, SSRC negociado e IP de origen (la IP TCP del
request HTTP); no hay "first packet wins". Sin RTCP ni FEC en v1-lite.

## Heartbeat y lease

`POST /receiver-lite/heartbeat` cada 10 s renueva el lease a 30 s (reloj
monotónico). Al vencer 30 s sin heartbeat, la sesión se cierra con el mismo
teardown que DELETE.

## Firewall / NAT

Red local confiable. Sin NAT traversal ni WAN.

## Seguridad (estado actual)

- Identidad Ed25519 persistente compatible con Michi Link.
- Pairing RECEIVER_BUTTON (ventana física de 120 s).
- Tokens de pairing: digest SHA-256 persistido; `session_token` solo en RAM.
- TLS, PAKE y cifrado de transporte son **trabajo futuro** (MS-12): el
  transporte actual no cifra el flujo RTP y la HTTP corre en LAN confiable.

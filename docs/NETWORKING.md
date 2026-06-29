# Michi Music Stream — Networking

## Conectividad

Wi-Fi Station. No opera como AP.

## Discovery

mDNS como `_michi-receiver._tcp`.

Atributos TXT: `device_id`, `type`, `api_version`, `firmware`, `name`.

## Transporte de audio

UDP raw hacia `stream_port`. Sin re-transmisión ni FEC en v1-lite.

## Heartbeat

POST cada 30 s. Timeout de seguridad: 90 s.

## Firewall / NAT

Red local confiable. Sin NAT traversal ni WAN.

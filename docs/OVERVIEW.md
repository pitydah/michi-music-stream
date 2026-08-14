# Michi Music Stream — Overview

## Qué es

Michi Music Stream es una familia de receptores físicos de audio diseñados para integrarse al ecosistema Michi. Su única función es recibir un flujo de audio por red y reproducirlo a través de una salida física (jack 3.5 mm o RCA).

## Qué no es

- No es una biblioteca musical.
- No administra playlists.
- No indexa música.
- No es un servidor musical.
- No tiene almacenamiento local de música.
- No reproduce de forma autónoma.

## Cómo se integra

Michi Music Stream es una salida de audio para el ecosistema. Para funcionar necesita:
1. Una red Wi-Fi local.
2. Un controlador emparejado (Michi Micro Server) que envíe comandos REST.
3. Una fuente de audio (Micro Server) que transmita audio por RTP/UDP.

## Variantes

| Aspecto | Standard | Hi-Fi |
|---------|----------|-------|
| MCU | ESP32-S3 | ESP32-S3 |
| DAC | I2S (PCM5102A) | DAC Hi-Fi (PCM5122) |
| Salida | Jack 3.5 mm | RCA estéreo |
| Audio certificado hoy | PCM 16-bit / 48 kHz / 2 canales | PCM 16-bit / 48 kHz / 2 canales |
| Codec certificado hoy | `pcm_s16le` | `pcm_s16le` |

> PCM 24-bit, 96 kHz y Opus son capacidades **futuras**: el silicio del DAC
> Hi-Fi las soporta, pero no hay camino implementado ni anunciado.

## Protocolo: Michi Link v1-lite

Contrato canónico publicado por Michi Link, vendorizado en
`contracts/michi-link/`. Rutas exactas bajo `/api/v1`:

| Método | Ruta | Auth |
|--------|------|------|
| GET | `/api/v1/server/info` | No |
| POST | `/api/v1/pair/start` | Ventana física abierta |
| GET | `/api/v1/pair/status` | No |
| POST | `/api/v1/pair/confirm` | Sesión de pairing |
| POST | `/api/v1/receiver-lite/session` | Bearer |
| GET | `/api/v1/receiver-lite/session` | Bearer |
| PATCH | `/api/v1/receiver-lite/session` | Bearer + sesión |
| DELETE | `/api/v1/receiver-lite/session` | Bearer + sesión |
| POST | `/api/v1/receiver-lite/heartbeat` | Bearer + sesión |

Extensiones opcionales: `now-playing` (PUT), `diagnostics` (GET),
`firmware` (GET/POST) — anunciadas por feature flags veraces.

Volumen como entero **0-100**, validado (sin clamp silencioso).

Formato de error canónico (una sola forma):
```json
{
  "error": {
    "code": "INVALID_REQUEST",
    "message": "Descripción legible",
    "request_id": "req_...",
    "details": {}
  }
}
```

Códigos: `INVALID_REQUEST` (400), `UNAUTHORIZED` (401), `FORBIDDEN` (403),
`NOT_FOUND` (404), `CONFLICT` (409), `RATE_LIMITED` (429),
`NOT_IMPLEMENTED` (501), `INTERNAL_ERROR` (500).

## Seguridad

- Identidad Ed25519 persistente; `michi_id` derivado de la clave pública.
- Pairing RECEIVER_BUTTON: botón físico abre ventana de 120 s; PIN de 6
  dígitos local; el receptor genera el token (32 bytes, base64url) y
  persiste solo su digest SHA-256 en NVS.
- `session_token` (32 bytes, base64url) solo existe en RAM y se entrega una vez.
- Factory reset: botón presionado >= 10 s (arm window de 10 s post-arranque;
  bloqueado en BOOTING/SELF_TEST/UPDATING). Recovery: pulsación de 5-10 s
  con el estado RECOVERABLE_ERROR.
- TLS, PAKE y hardening de producción son trabajo futuro (MS-12).

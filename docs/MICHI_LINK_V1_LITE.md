# Michi Link v1-lite — Especificación del Protocolo

## Introducción

Protocolo REST liviano para control de receptores Michi Music Stream. Corre sobre HTTP en puerto 80. Audio via UDP.

## Detalles

| Aspecto | Valor |
|---------|-------|
| Base URL | `http://{ip}:80/api/v1` |
| Transporte | HTTP REST |
| Eventos | No soportados en v1-lite |
| Discovery | mDNS `_michi-receiver._tcp` |
| Audio | UDP raw |
| Auth | `Authorization: Bearer <token>` |
| Content-Type | `application/json` |

## Formato de error

```json
{
  "error": {
    "code": "error_code",
    "message": "Descripción.",
    "details": {}
  }
}
```

### Códigos

| Código | HTTP | Significado |
|--------|------|-------------|
| `invalid_token` | 401 | Token ausente o inválido |
| `bad_request` | 400 | Payload malformado |
| `not_found` | 404 | Endpoint inexistente |
| `pairing_window_closed` | 409 | No hay ventana abierta |
| `pairing_window_open` | 409 | Ya hay ventana activa |
| `already_paired` | 409 | Ya emparejado con este controller |
| `session_active` | 409 | Ya hay sesión activa |
| `unsupported_codec` | 400 | Codec no soportado |
| `unsupported_rate` | 400 | Sample rate no soportado |
| `internal_error` | 500 | Error interno |

## Endpoints

### GET /api/v1/receiver/info

Sin auth. Devuelve JSON completo del dispositivo (ver ejemplos).

### GET /api/v1/receiver/firmware

Sin auth.

```json
{
  "device_id": "rcv_standard_001",
  "current_version": "0.1.0",
  "build_date": "2026-06-29",
  "ota_supported": false
}
```

### POST /api/v1/receiver/pair/start

Sin auth. Inicia ventana de 120 s.

**Request:**
```json
{
  "initiator": "michi_music_player",
  "initiator_id": "player_001"
}
```

**Response (200):**
```json
{
  "status": "pairing_window_open",
  "device_id": "rcv_standard_001",
  "pairing_window_seconds": 120,
  "nonce": "a1b2c3d4"
}
```

**Error (409):** `pairing_window_open` o `already_paired`.

### POST /api/v1/receiver/pair/confirm

Sin auth (usa nonce como prueba).

**Request:**
```json
{
  "nonce": "a1b2c3d4",
  "initiator_id": "player_001",
  "token": "tok_michi_stream_player_abc123"
}
```

**Response (200):**
```json
{
  "status": "paired",
  "device_id": "rcv_standard_001",
  "controller_id": "player_001",
  "token": "tok_michi_stream_player_abc123"
}
```

Token almacenado en NVS. **Error (409):** `pairing_window_closed`.

### POST /api/v1/receiver/heartbeat

Requiere auth. Latido de sesión cada 30 s.

**Request:**
```json
{ "session_id": "sess_001" }
```

**Response (200):**
```json
{
  "status": "alive",
  "session_id": "sess_001",
  "uptime_seconds": 30
}
```

Sin heartbeat por 90 s → sesión detenida automáticamente.

### POST /api/v1/receiver/session/start

Requiere auth.

**Request (Standard):**
```json
{
  "session_id": "sess_001",
  "codec": "pcm_s16le",
  "sample_rate": 48000,
  "bit_depth": 16,
  "channels": 2,
  "transport": "udp",
  "stream_port": 55300,
  "buffer_ms": 250,
  "volume": 70
}
```

| Campo | Rango | Descripción |
|-------|-------|-------------|
| `session_id` | 1-32 chars | ID único |
| `codec` | — | `pcm_s16le`, `pcm_s24le`, `opus` |
| `sample_rate` | Según hw | Máx. 48000 Std, 96000 Hi-Fi |
| `bit_depth` | 16 o 24 | Según hw |
| `channels` | 1-2 | Mono o estéreo |
| `transport` | — | Solo `udp` |
| `stream_port` | 1024-65535 | Puerto UDP para audio |
| `buffer_ms` | 50-2000 | Buffer de jitter |
| `volume` | 0-100 | Volumen inicial |

**Response (200):**
```json
{
  "status": "session_started",
  "session_id": "sess_001",
  "device_id": "rcv_standard_001",
  "stream_port": 55300,
  "buffer_ms": 250
}
```

**Error (400):** `unsupported_codec`, `unsupported_rate`. **Error (409):** `session_active`.

### POST /api/v1/receiver/session/stop

Requiere auth.

```json
{ "session_id": "sess_001" }
```

**Response (200):** `{"status": "session_stopped", "session_id": "sess_001"}`

### POST /api/v1/receiver/volume

Requiere auth. Rango 0-100.

```json
{ "volume": 50 }
```

**Response (200):** `{"status": "volume_set", "volume": 50}`

## Autenticación

1. Controlador genera token durante pairing.
2. Receptor lo guarda en NVS.
3. Toda petición POST (excepto pair/start, pair/confirm) requiere:
   ```
   Authorization: Bearer <token>
   ```
4. Token inválido → 401 `invalid_token`.

## Transporte de audio

- `pcm_s16le`: 2 bytes/muestra LE, interleaved, sin cabecera.
- `pcm_s24le`: 3 bytes/muestra LE, interleaved, sin cabecera.
- `opus`: trama Opus raw (sin Ogg), 20 ms recomendado.
- Paquete recomendado: 10 ms de audio.

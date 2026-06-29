# Michi Link v1-lite — Especificación del Protocolo

Protocolo REST liviano para control de receptores Michi Music Stream. Corre sobre HTTP en puerto 80. Audio via UDP.

## Detalles técnicos

| Aspecto | Valor |
|---------|-------|
| Base URL | `http://{ip}:80/api/v1` |
| Transporte principal | HTTP REST |
| Eventos | No soportados en v1-lite |
| Discovery | mDNS `_michi-receiver._tcp` |
| Audio | UDP raw |
| Autenticación | `Authorization: Bearer <token>` |
| Estrategia de auth | `RECEIVER_BUTTON` (pairing con botón físico) |
| Content-Type | `application/json` |
| Volumen | Entero `0` (silencio) a `100` (máximo) |

## Roles del dispositivo

| Role | Descripción |
|------|-------------|
| `audio_receiver` | Receptor de audio por red |
| `music_stream_receiver` | Reproductor de flujo continuo |

## Formato de error

Todas las respuestas de error siguen esta estructura:

```json
{
  "error": {
    "code": "codigo_maquina",
    "message": "Descripcion legible para debugging.",
    "details": {}
  }
}
```

### Códigos de error

| Código | HTTP | Cuándo ocurre |
|--------|------|---------------|
| `invalid_token` | 401 | Token ausente, mal formado o inválido |
| `bad_request` | 400 | Payload JSON inválido o campos faltantes |
| `not_found` | 404 | Endpoint no existe |
| `pairing_window_closed` | 409 | Se intentó confirmar pairing sin ventana abierta |
| `pairing_window_open` | 409 | Ya hay una ventana de pairing activa |
| `session_active` | 409 | Ya hay una sesión de audio en curso |
| `unsupported_codec` | 400 | Codec no soportado por este hardware |
| `unsupported_rate` | 400 | Sample rate excede el máximo del hardware |
| `internal_error` | 500 | Error interno del dispositivo |

---

## Endpoints

### GET /api/v1/receiver/info

Sin autenticación. Devuelve información completa del dispositivo.

Respuesta (200) — ver `examples/receiver-standard-info.json` y `examples/receiver-hifi-info.json`.

---

### GET /api/v1/receiver/firmware

Sin autenticación.

Respuesta (200):
```json
{
  "device_id": "rcv_standard_001",
  "current_version": "0.1.0",
  "build_date": "2026-06-29",
  "ota_supported": false
}
```

---

### POST /api/v1/receiver/pair/start

Sin autenticación. Inicia ventana de pairing de 120 segundos.
Requiere que el botón físico haya sido presionado previamente.
Si no hay botón presionado, el receptor rechaza con `pairing_window_closed`.

Request:
```json
{
  "initiator": "michi_music_player",
  "initiator_id": "player_001"
}
```

| Campo | Tipo | Descripción |
|-------|------|-------------|
| `initiator` | string | Nombre del controlador |
| `initiator_id` | string | ID único del controlador |

Response (200):
```json
{
  "status": "pairing_window_open",
  "device_id": "rcv_standard_001",
  "pairing_window_seconds": 120,
  "nonce": "a1b2c3d4"
}
```

Error (409): `pairing_window_open` si ya hay ventana activa.

---

### POST /api/v1/receiver/pair/confirm

Sin autenticación (usa el nonce como prueba de posesión de la ventana).

Request:
```json
{
  "nonce": "a1b2c3d4",
  "initiator_id": "player_001",
  "token": "tok_michi_stream_player_abc123"
}
```

| Campo | Tipo | Descripción |
|-------|------|-------------|
| `nonce` | string | Nonce recibido en pair/start |
| `initiator_id` | string | ID del controlador |
| `token` | string | Token generado por el controlador |

El token se almacena en NVS (persiste entre reinicios).

Response (200):
```json
{
  "status": "paired",
  "device_id": "rcv_standard_001",
  "controller_id": "player_001",
  "token": "tok_michi_stream_player_abc123"
}
```

Error (409): `pairing_window_closed` si la ventana expiró o no fue abierta.

---

### POST /api/v1/receiver/heartbeat

Requiere `Authorization: Bearer <token>`.

Latido periódico que mantiene viva la sesión de audio.

Request:
```json
{ "session_id": "sess_001" }
```

Response (200):
```json
{
  "status": "alive",
  "session_id": "sess_001",
  "uptime_seconds": 30
}
```

Intervalo recomendado entre heartbeats: 30 segundos.
Timeout de seguridad: 90 segundos sin heartbeat → sesión detenida automáticamente.

---

### POST /api/v1/receiver/session/start

Requiere `Authorization: Bearer <token>`.

Inicia una sesión de audio. Solo se permite una sesión activa a la vez.

Request:
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

| Campo | Tipo | Rango / Valores | Descripción |
|-------|------|-----------------|-------------|
| `session_id` | string | 1-32 caracteres | ID único de la sesión |
| `codec` | string | `pcm_s16le`, `pcm_s24le`, `opus` | Códec del flujo |
| `sample_rate` | int | ≤ máximo del hardware | Frecuencia de muestreo en Hz |
| `bit_depth` | int | ≤ máximo del hardware | Profundidad de bits (16 o 24) |
| `channels` | int | 2 | Solo estéreo en v1-lite |
| `transport` | string | `udp` | Protocolo de transporte |
| `stream_port` | int | 1024-65535 | Puerto UDP donde se recibirá audio |
| `buffer_ms` | int | 50-2000 | Tamaño del buffer de jitter en ms |
| `volume` | int | 0-100 | Volumen inicial |

Validaciones del receptor:
- `codec` debe estar en `supported_codecs`.
- `sample_rate` ≤ `max_sample_rate` del hardware.
- `bit_depth` ≤ `max_bit_depth` del hardware.
- `channels` debe ser 2.
- `volume` debe estar entre 0 y 100.
- No debe existir una sesión activa previa.

Response (200):
```json
{
  "status": "session_started",
  "session_id": "sess_001",
  "device_id": "rcv_standard_001",
  "stream_port": 55300,
  "buffer_ms": 250
}
```

Error (400): `unsupported_codec`, `unsupported_rate`.
Error (409): `session_active` si ya hay sesión en curso.

---

### POST /api/v1/receiver/session/stop

Requiere `Authorization: Bearer <token>`.

Detiene la sesión de audio activa.

Request:
```json
{ "session_id": "sess_001" }
```

Response (200):
```json
{
  "status": "session_stopped",
  "session_id": "sess_001"
}
```

---

### POST /api/v1/receiver/volume

Requiere `Authorization: Bearer <token>`.

Ajusta el volumen de la salida de audio. Rango 0-100.

Request:
```json
{ "volume": 50 }
```

Response (200):
```json
{
  "status": "volume_set",
  "volume": 50
}
```

Si `volume` está fuera de rango (menor a 0 o mayor a 100), el receptor debe truncar al límite más cercano sin devolver error.

---

## Autenticación

1. El usuario presiona el botón físico de pairing en el receptor → se abre ventana de 120 s.
2. El controlador envía `POST /api/v1/receiver/pair/start`.
3. El controlador envía `POST /api/v1/receiver/pair/confirm` con un token propio.
4. El receptor almacena el token en NVS (memoria persistente).
5. Toda petición POST posterior (excepto `pair/start` y `pair/confirm`) debe incluir:
   ```
   Authorization: Bearer <token>
   ```
6. Token inválido o ausente → respuesta 401 `invalid_token`.

Estrategia de autenticación documentada: `RECEIVER_BUTTON`.

---

## Transporte de audio

Una vez iniciada la sesión, el controlador envía audio por UDP al `stream_port` indicado.

| Codec | Formato |
|-------|---------|
| `pcm_s16le` | 2 bytes/muestra, little-endian, interleaved L/R, sin cabecera |
| `pcm_s24le` | 3 bytes/muestra, little-endian, interleaved L/R, sin cabecera |
| `opus` | Trama Opus raw (sin contenedor Ogg), 20 ms por paquete |

Tamaño de paquete recomendado: 10 ms de audio.

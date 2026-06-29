# Michi Link v1-lite — Especificación del Protocolo

Protocolo REST liviano para control de receptores Michi Music Stream. Corre sobre HTTP en puerto 80. Audio via UDP.

## Versión

| Campo | Valor |
|-------|-------|
| `michi_link_version` | `1.0.0-alpha` |
| `api_version` | `v1` |
| API base path | `/api/v1` |

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
| Token refresh | `false` (token fijo hasta factory reset) |
| Content-Type | `application/json` |
| Volumen | Entero `0` (silencio) a `100` (máximo) |

## `receiver/info` — Esquema completo

| Campo | Tipo | Obligatorio | Descripción |
|-------|------|-------------|-------------|
| `service` | string | sí | Identificador del servicio: `michi-stream-standard` o `michi-stream-hifi` |
| `name` | string | sí | Nombre legible configurable |
| `device_id` | string | sí | Identificador único del dispositivo |
| `api_version` | string | sí | Versión de API REST: `v1` |
| `michi_link_version` | string | sí | Versión del protocolo Michi Link: `1.0.0-alpha` |
| `firmware` | string | sí | Versión de firmware |
| `type` | string | sí | `michi_stream_standard` o `michi_stream_hifi` |
| `roles` | array | sí | Roles del dispositivo |
| `auth` | object | sí | Configuración de autenticación |
| `auth.required` | bool | sí | `true` si requiere token |
| `auth.strategy` | string | sí | `RECEIVER_BUTTON` |
| `auth.token_refresh` | bool | sí | `false` (token fijo) |
| `output` | object | sí | Capacidades de salida de audio |
| `supported_codecs` | array | sí | Codecs de audio soportados |
| `features` | object | sí | Características del dispositivo |

### Roles

| Role | Descripción |
|------|-------------|
| `audio_receiver` | Receptor de audio por red |
| `music_stream_receiver` | Reproductor de flujo continuo |

### `output`

| Campo | Tipo | Descripción |
|-------|------|-------------|
| `connector` | string | Tipo de conector físico: `jack_3_5`, `rca_stereo` |
| `dac` | string | (opcional) Identificador del DAC: `hifi_i2s` |
| `max_sample_rate` | int | Frecuencia de muestreo máxima en Hz |
| `max_bit_depth` | int | Profundidad de bits máxima |
| `channels` | int | Número de canales (siempre 2 en v1-lite) |

### `auth`

```json
"auth": {
  "required": true,
  "strategy": "RECEIVER_BUTTON",
  "token_refresh": false
}
```

### `features`

| Feature | Tipo | Descripción |
|---------|------|-------------|
| `pairing_button` | bool | Botón físico de pairing presente |
| `volume` | bool | Control de volumen digital soportado |
| `heartbeat` | bool | Heartbeat de sesión soportado |
| `ota_update` | bool | Actualización OTA soportada |

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

Sin autenticación. Devuelve información completa del dispositivo según el esquema anterior.

Ver ejemplos en `examples/receiver-standard-info.json` y `examples/receiver-hifi-info.json`.

---

### GET /api/v1/receiver/firmware

Sin autenticación.

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

Error (409) con `details`:
```json
{
  "error": {
    "code": "pairing_window_open",
    "message": "Ya hay una ventana de pairing activa.",
    "details": {
      "remaining_seconds": 85
    }
  }
}
```

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

Error (409):
```json
{
  "error": {
    "code": "pairing_window_closed",
    "message": "La ventana de pairing expiró o no fue abierta. Presione el botón físico.",
    "details": {}
  }
}
```

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

Error (400) — codec no soportado:
```json
{
  "error": {
    "code": "unsupported_codec",
    "message": "Codec 'pcm_s24le' no soportado. Codecs: pcm_s16le, opus.",
    "details": {
      "requested_codec": "pcm_s24le",
      "supported_codecs": ["pcm_s16le", "opus"]
    }
  }
}
```

Error (409) — sesión activa:
```json
{
  "error": {
    "code": "session_active",
    "message": "Ya hay una sesion de audio activa.",
    "details": {
      "active_session_id": "sess_001"
    }
  }
}
```

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
4. El receptor almacena el token en NVS (memoria persistente entre reinicios).
5. Toda petición POST posterior (excepto `pair/start` y `pair/confirm`) debe incluir:
   ```
   Authorization: Bearer <token>
   ```
6. Token inválido o ausente → respuesta 401 `invalid_token`.

Estrategia de autenticación: `RECEIVER_BUTTON`.
El token no se refresca automáticamente. Para renovar, se debe hacer factory reset y repetir el pairing.

---

## Transporte de audio

Una vez iniciada la sesión, el controlador envía audio por UDP al `stream_port` indicado.

| Codec | Formato |
|-------|---------|
| `pcm_s16le` | 2 bytes/muestra, little-endian, interleaved L/R, sin cabecera |
| `pcm_s24le` | 3 bytes/muestra, little-endian, interleaved L/R, sin cabecera |
| `opus` | Trama Opus raw (sin contenedor Ogg), 20 ms por paquete |

Tamaño de paquete recomendado: 10 ms de audio.

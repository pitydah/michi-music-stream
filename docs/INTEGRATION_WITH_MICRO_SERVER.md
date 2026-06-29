# Integración con Michi Micro Server

Este documento describe cómo Michi Micro Server descubre, empareja y usa un Michi Music Stream como salida de audio. Es la guía de integración para quien implemente el lado Micro Server del protocolo v1-lite.

## Arquitectura

```
                   ┌──────────────────────┐
                   │   Michi Music Mobile │
                   │   (control remoto)   │
                   └──────────┬───────────┘
                              │ HTTP REST
                              ▼
                   ┌──────────────────────┐
                   │  Michi Micro Server  │
                   │  (orquestador)       │
                   └──┬───────────────┬───┘
                      │               │
                      │ 1. mDNS       │ 4. POST /api/v1/receiver/session/start
                      │    discover    │ 5. UDP audio stream (port 55300)
                      ▼               ▼
             ┌─────────────────────────────────┐
             │      Michi Music Stream         │
             │  (Standard o Hi-Fi)             │
             │  http://<stream-ip>:80/api/v1   │
             └─────────────────────────────────┘
```

- **Mobile** solo controla Micro Server. Mobile no se comunica directamente con el Stream.
- **Micro Server** orquesta: descubre, empareja, inicia sesiones, envía heartbeat, controla volumen, detiene sesiones.
- **Michi Stream** es una salida física sin conocimiento de biblioteca, playlists, metadatos ni colas.

## Mapeo de acciones de Micro Server a endpoints REST

| Acción de Micro Server | Endpoint en Stream | Request | Response esperada |
|------------------------|--------------------|---------|-------------------|
| Descubrir receptores | mDNS `_michi-receiver._tcp` | — | TXT records con device_id, type, api_version, firmware, name |
| Obtener perfil completo | `GET /api/v1/receiver/info` | — | JSON con service, name, device_id, api_version, michi_link_version, roles, auth, output, supported_codecs, features |
| Iniciar pairing | `POST /api/v1/receiver/pair/start` | `{initiator, initiator_id}` | `{status, device_id, pairing_window_seconds, nonce}` |
| Confirmar pairing | `POST /api/v1/receiver/pair/confirm` | `{nonce, initiator_id, token}` | `{status, device_id, controller_id, token}` |
| Iniciar sesión de audio | `POST /api/v1/receiver/session/start` | `{session_id, codec, sample_rate, bit_depth, channels, transport, stream_port, buffer_ms, volume}` | `{status, session_id, device_id, stream_port, buffer_ms}` |
| Enviar heartbeat | `POST /api/v1/receiver/heartbeat` | `{session_id}` | `{status, session_id, uptime_seconds}` |
| Ajustar volumen | `POST /api/v1/receiver/volume` | `{volume}` | `{status, volume}` |
| Detener sesión | `POST /api/v1/receiver/session/stop` | `{session_id}` | `{status, session_id}` |
| Consultar firmware | `GET /api/v1/receiver/firmware` | — | `{device_id, current_version, build_date, ota_supported}` |

## Flujo paso a paso

### 1. Descubrimiento

Micro Server escucha mDNS en `_michi-receiver._tcp`. Cuando aparece un nuevo servicio, lee los TXT records y opcionalmente consulta `GET /api/v1/receiver/info` para el perfil completo.

Micro Server mantiene una lista interna de receptores conocidos. Cada entrada incluye:
- `device_id`, `type`, `api_version`, `firmware`, `name` (de mDNS)
- `ip` (de la resolución mDNS)
- `paired` (bool), `token` (si está emparejado)

### 2. Pairing

El usuario presiona el botón físico en el receptor. Esto abre una ventana de 120 segundos.

```
Micro Server                         Michi Stream
     │                                     │
     │  POST /api/v1/receiver/pair/start   │
     │  {initiator: "michi_micro_server",  │
     │   initiator_id: "micro_001"}        │
     │────────────────────────────────────>│
     │                                     │── validates window open
     │  {status: "pairing_window_open",    │
     │   device_id: "rcv_standard_001",    │
     │   pairing_window_seconds: 120,      │
     │   nonce: "a1b2c3d4"}                │
     │<────────────────────────────────────│
     │                                     │
     │  POST /api/v1/receiver/pair/confirm │
     │  {nonce: "a1b2c3d4",               │
     │   initiator_id: "micro_001",        │
     │   token: "tok_micro_abc123"}        │
     │────────────────────────────────────>│
     │                                     │── valida nonce, guarda en NVS
     │  {status: "paired",                 │
     │   device_id: "rcv_standard_001",    │
     │   controller_id: "micro_001",       │
     │   token: "tok_micro_abc123"}        │
     │<────────────────────────────────────│
```

A partir de este momento, toda petición POST debe incluir `Authorization: Bearer tok_micro_abc123`.

### 3. Inicio de sesión de audio

```
Micro Server                         Michi Stream
     │                                     │
     │  POST /api/v1/receiver/session/start│
     │  Authorization: Bearer tok_...      │
     │  {session_id: "sess_001",           │
     │   codec: "pcm_s16le",               │
     │   sample_rate: 48000,               │
     │   bit_depth: 16,                    │
     │   channels: 2,                      │
     │   transport: "udp",                 │
     │   stream_port: 55300,               │
     │   buffer_ms: 250,                   │
     │   volume: 70}                       │
     │────────────────────────────────────>│
     │                                     │── valida codec, rate, depth,
     │                                     │   channels, volume, sesión única
     │  {status: "session_started",        │
     │   session_id: "sess_001",           │
     │   device_id: "rcv_standard_001",    │
     │   stream_port: 55300,               │
     │   buffer_ms: 250}                   │
     │<────────────────────────────────────│
     │                                     │
     │  UDP audio (pcm_s16le, port 55300)  │
     │════════════════════════════════════>│── buffer circular + I2S DAC
```

### 4. Heartbeat

Micro Server envía `POST /api/v1/receiver/heartbeat` cada 30 segundos.

Si Micro Server deja de enviar heartbeat por 90 segundos, el receptor:
1. Cierra la sesión de audio.
2. Libera el puerto UDP.
3. Queda disponible para una nueva sesión.

### 5. Control de volumen

```
Mobile ──REST──> Micro Server ──POST /api/v1/receiver/volume──> Stream
```

El volumen viaja como entero 0-100 a través de toda la cadena.

### 6. Fin de sesión

Micro Server envía `POST /api/v1/receiver/session/stop`. Esto ocurre cuando:
- El usuario detiene la reproducción desde Mobile.
- Micro Server cambia a otra salida.
- Micro Server se apaga (graceful shutdown).

## Lo que el receptor NO necesita saber

| Concepto | Dónde vive |
|----------|-----------|
| Biblioteca musical | Micro Server |
| Playlists | Micro Server |
| Indexación | Micro Server |
| Metadatos de canciones | Micro Server |
| Cola de reproducción | Micro Server |
| Lógica de multiroom | Micro Server |
| UI/UX de control | Mobile |
| Transcodificación | Micro Server |
| Gestión de usuarios | Micro Server |

El receptor solo recibe y procesa:
- Comandos REST (pairing, sesión, heartbeat, volumen).
- Paquetes UDP de audio.

## Consideraciones de red

- Micro Server y Michi Stream deben estar en la misma subred.
- Se recomienda Wi-Fi 5 GHz para 96 kHz / 24-bit.
- Para 48 kHz / 16-bit, 2.4 GHz es suficiente en redes no congestionadas.
- El buffer de jitter del receptor debe ser ≥ 2× la latencia promedio de ida y vuelta.
- Micro Server debe calcular el tamaño de paquete UDP como 10 ms de audio:

| Codec | Canales | Bytes/paquete (10 ms) |
|-------|---------|----------------------|
| pcm_s16le @ 48 kHz | 2 | 1920 |
| pcm_s24le @ 96 kHz | 2 | 5760 |
| opus @ 48 kHz | 2 | ~300 (variable) |

## Estados del receptor visibles para Micro Server

| Estado | Cómo lo detecta Micro Server |
|--------|------------------------------|
| Disponible | mDNS presente, no hay sesión activa |
| En pairing | Se recibió `pairing_window_open` |
| Emparejado | Token almacenado, `pair/confirm` exitoso |
| Sesión activa | `session/start` exitoso, heartbeats responden |
| Sesión terminada por timeout | Heartbeat deja de responder por 90+ segundos |
| Factory reset | Token deja de funcionar, nuevo pairing requerido |

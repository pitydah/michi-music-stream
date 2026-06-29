# Integración con Michi Micro Server

Este documento describe cómo Michi Micro Server descubre, empareja y usa un Michi Music Stream como salida de audio.

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
                     │ mDNS discover │ POST /api/v1/receiver/session/start
                     ▼               │ UDP audio stream
            ┌────────────────┐       ▼
            │ Michi Stream   │◄──────────────
            │ (Standard/HiFi)│   raw PCM/Opus
            └────────────────┘
```

- **Mobile** solo controla Micro Server vía REST. Mobile no se comunica directamente con el Stream.
- **Micro Server** descubre, empareja, inicia sesiones y envía audio.
- **Michi Stream** es una salida física sin conocimiento de biblioteca ni playlists.

## Flujo de integración

### 1. Descubrimiento

Micro Server escucha mDNS en la red local buscando servicios `_michi-receiver._tcp`.

Atributos TXT expuestos:
- `device_id`
- `type` (`michi_stream_standard` o `michi_stream_hifi`)
- `api_version` (`v1-lite`)
- `firmware`
- `name`

Micro Server puede consultar `GET /api/v1/receiver/info` para obtener el perfil completo.

### 2. Registro del receptor

Micro Server mantiene una lista interna de receptores conocidos. Cuando aparece un nuevo receptor por mDNS, puede:
- Mostrarlo en la interfaz de Mobile como "salida disponible".
- Esperar a que el usuario presione el botón de pairing.

### 3. Pairing

1. Usuario presiona botón físico en el receptor (ventana de 120 segundos).
2. Micro Server envía `POST /api/v1/receiver/pair/start`.
3. Micro Server envía `POST /api/v1/receiver/pair/confirm` con un token.
4. El receptor almacena el token en NVS. A partir de este momento, solo peticiones
   con `Authorization: Bearer <token>` serán aceptadas (excepto info y firmware).

### 4. Inicio de sesión de audio

1. Micro Server envía `POST /api/v1/receiver/session/start` con codec, sample rate,
   stream_port y volumen.
2. El receptor abre un socket UDP en `stream_port` y comienza a acumular buffer.
3. Micro Server comienza a enviar paquetes UDP con audio PCM/Opus.

### 5. Heartbeat

Micro Server envía `POST /api/v1/receiver/heartbeat` cada 30 segundos.
Si Micro Server deja de enviar heartbeat por 90 segundos, el receptor detiene la sesión.

### 6. Control de volumen

Mobile → REST → Micro Server → `POST /api/v1/receiver/volume`.

El volumen se expresa como entero 0-100.

### 7. Fin de sesión

Micro Server envía `POST /api/v1/receiver/session/stop`.

Ocurre cuando:
- El usuario detiene la reproducción desde Mobile.
- Micro Server cambia a otra salida.
- Micro Server se apaga.

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

El receptor solo recibe:
- Comandos REST (pairing, sesión, volumen).
- Paquetes UDP de audio.

## Consideraciones de red

- Micro Server y Michi Stream deben estar en la misma subred.
- Se recomienda una red Wi-Fi estable de 5 GHz para 96 kHz / 24-bit.
- Para 48 kHz / 16-bit, 2.4 GHz es suficiente si no hay congestión.
- El buffer de jitter del receptor debe ser ≥ 2× la latencia promedio de la red.

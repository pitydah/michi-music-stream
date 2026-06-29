# Michi Music Stream Simulator

Simulador HTTP del protocolo Michi Link v1-lite para receptores Michi Music Stream. Permite que Micro Server integre receivers sin depender de hardware ESP32 real.

## Requisitos

```bash
pip install flask
```

## Uso rápido

```bash
# Standard (puerto 8080)
python3 receiver_sim.py --type standard

# Hi-Fi (puerto 8081)
python3 receiver_sim.py --type hifi --port 8081

# Configuración personalizada
python3 receiver_sim.py --config mi_config.json
```

## Endpoints simulados

| Método | Ruta | Auth | Descripción |
|--------|------|------|-------------|
| GET | `/api/v1/receiver/info` | No | Información del dispositivo |
| GET | `/api/v1/receiver/firmware` | No | Versión de firmware |
| POST | `/api/v1/receiver/pair/start` | No | Inicia ventana de pairing (120 s) |
| POST | `/api/v1/receiver/pair/confirm` | No | Confirma emparejamiento |
| POST | `/api/v1/receiver/heartbeat` | Bearer | Latido de sesión |
| POST | `/api/v1/receiver/session/start` | Bearer | Inicia sesión de audio |
| POST | `/api/v1/receiver/session/stop` | Bearer | Detiene sesión |
| POST | `/api/v1/receiver/volume` | Bearer | Ajusta volumen (0-100) |

## Comportamiento simulado

| Feature | Comportamiento |
|---------|---------------|
| Pairing window | 120 segundos, nonce criptográfico, hasta 4 controladores |
| Sesión activa | Una sola sesión; duplicado → 409 |
| Heartbeat | Timeout de 90 s (sesión se auto-detiene) |
| Volumen | 0-100, clamping automático |
| Codec inválido | 400 unsupported_codec con details |
| Sample rate inválido | 400 unsupported_rate con details |
| Auth | Bearer token; inválido → 401 |

## Logging

El simulador genera logs estructurados con timestamp ISO8601 para:

- `Session STARTED: id=... codec=...`
- `Session STOPPED: id=...`
- `Heartbeat received (uptime=...s)`
- `Volume changed to ...`
- `Pairing window OPEN (nonce=...)`
- `Pairing CONFIRMED: controller=... token=...`
- `Auth FAILED for ... (token=...)`

## Configuración

Ver `config.example.json`. Se puede personalizar:
- `device_id`, `name`
- `output.max_sample_rate`, `output.max_bit_depth`
- `supported_codecs`
- `features`

## Tests

```bash
cd simulator
python3 -m pytest tests/ -v
```

## Limitaciones

- No envía ni recibe audio UDP real.
- No implementa mDNS (el script no se anuncia en la red).
- No persiste estado entre reinicios.
- No implementa timeout de heartbeat automático (el caller debe esperar 90 s).

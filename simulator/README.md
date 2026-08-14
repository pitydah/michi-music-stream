# Michi Music Stream Simulator

Simulador HTTP canónico del protocolo Michi Link v1-lite para receptores Michi Music Stream. Permite que Micro Server integre receivers sin depender de hardware ESP32 real. Implementa EXCLUSIVAMENTE el contrato publicado en `contracts/michi-link/`; las rutas del dialecto legacy no existen.

## Requisitos

```bash
pip install -r requirements.txt
```

## Uso rápido

```bash
# Standard (puerto 8080)
python3 receiver_sim.py --type standard

# Hi-Fi (puerto 8081)
python3 receiver_sim.py --type hifi --port 8081

# Configuración personalizada
python3 receiver_sim.py --config mi_config.json

# Modos iniciales
python3 receiver_sim.py --type standard --pairing-open
python3 receiver_sim.py --type standard --active-session
python3 receiver_sim.py --type standard --fail-heartbeat
```

## Endpoints simulados (canónicos)

| Método | Ruta | Auth | Descripción |
|--------|------|------|-------------|
| GET | `/api/v1/server/info` | No | Perfil canónico (identidad del bundle) |
| POST | `/api/v1/pair/start` | No; ventana abierta | Crea sesión de pairing; PIN local |
| GET | `/api/v1/pair/status` | No | Estado por `session_id` |
| POST | `/api/v1/pair/confirm` | No | Confirma PIN; token emitido por el receptor |
| POST | `/api/v1/receiver-lite/session` | Bearer | Sesión RTP (201; puerto 49152–65535) |
| GET | `/api/v1/receiver-lite/session` | Bearer | Estado de la sesión |
| PATCH | `/api/v1/receiver-lite/session` | Bearer + sesión | `volume` / `paused` |
| DELETE | `/api/v1/receiver-lite/session` | Bearer + sesión | Cierre (204) |
| POST | `/api/v1/receiver-lite/heartbeat` | Bearer + sesión | Renueva lease de 30 s |

La identidad es una fixture determinista (el vector de identidad del bundle);
la ventana de pairing se abre solo por hook interno; el token de pairing lo
genera el receptor (32 bytes, base64url) y solo se guarda su digest.

## Modos iniciales (flags CLI)

| Flag | Efecto |
|------|--------|
| `--pairing-open` | Inicia con ventana de pairing abierta (120 s) |
| `--pairing-closed` | (default) Ventana cerrada: `pair/start` responde 403 |
| `--active-session` | Inicia con controlador emparejado + sesión activa (tokens impresos en el log) |
| `--fail-heartbeat` | Igual que `--active-session` pero el lease ya expiró |

## Comportamiento simulado

| Feature | Comportamiento |
|---------|---------------|
| Pairing window | 120 s; PIN de 6 dígitos local; máx. 5 intentos → 429 |
| Sesión activa | Una sola sesión; duplicado → 409 |
| Lease | 30 s renovable por heartbeat; expiry cierra como DELETE |
| Volumen | 0-100, validado (101 → 400, no clamp) |
| Cuerpo inválido | 400 INVALID_REQUEST con `details.field` |
| Auth | Bearer; inválido → 401 |

## Logging

El simulador genera logs estructurados con timestamp ISO8601 (pairing, sesión,
heartbeat, auth). Nunca registra secretos: el PIN y el `session_token` solo se
imprimen cuando un modo inicial los genera para uso manual.

## Configuración

Ver `config.example.json`. Se puede personalizar: `service`, `name`,
`server_id`, `version`, `michi_id`, `public_key`, `features`, `audio`.

## Tests

```bash
# Suite completa del proyecto (sync + simulador + contrato + E2E)
./scripts/run_tests.sh

# Solo el simulador
python3 -m pytest simulator/tests -q
```

`simulator/tests/test_simulator.py` (29 tests), `test_scenarios.py` (10) y
`test_integration_http.py` (18) cubren el contrato canónico completo.

## Limitaciones

- No envía ni recibe audio UDP real.
- No implementa mDNS ni multicast firmado (el script no se anuncia en la red).
- No persiste estado entre reinicios.

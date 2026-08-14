# Michi Music Stream — Receiver Simulator

## Propósito

El `simulator/` permite probar la integración de receptores Michi Music Stream con Micro Server sin depender de hardware ESP32 real. Implementa el contrato canónico v1-lite (bundle `contracts/michi-link/`) sobre HTTP.

## Arquitectura

```
Michi Micro Server  ──HTTP──>  receiver_sim.py (puerto 8080)
  (integración real)           Simula Standard o Hi-Fi (contrato canónico)
```

El simulator se comporta como un receptor físico pero sin audio real ni puerto UDP.

## Capacidades

| Capacidad | Estado |
|-----------|--------|
| `GET /api/v1/server/info` Standard y Hi-Fi (identidad del bundle) | Completo |
| Pairing RECEIVER_BUTTON (ventana 120 s, PIN local, token del receptor) | Completo |
| `POST /receiver-lite/session` con validación exacta (PT 97, 48/16/2) | Completo |
| Heartbeat + lease 30 s (replay → 409, expiry → cierre) | Completo |
| Volumen 0-100 vía PATCH (sin clamp silencioso) | Completo |
| Error canónico `{error:{code,message,request_id,details}}` | Completo |
| Auth Bearer + `X-Michi-Session` | Completo |
| Validación de cada respuesta contra el bundle | Completo |
| mDNS `_michi-link._tcp` / multicast firmado | No simulado |
| Audio UDP real | No simulado (solo valida el contrato) |

## Uso

```bash
# Standard
python3 receiver_sim.py --type standard --port 8080

# Hi-Fi
python3 receiver_sim.py --type hifi --port 8081

# Con configuración personalizada
python3 receiver_sim.py --config mi_config.json
```

## Tests

```bash
cd simulator
python3 -m pytest tests -q
```

29 tests en `test_simulator.py` (info, identidad, pairing completo con
vectores del bundle, sesión, heartbeat/lease, volumen), 10 escenarios de
comportamiento en `test_scenarios.py` y 18 tests HTTP en
`test_integration_http.py`.

## Logs

El simulator genera logs con timestamp ISO8601 para cada evento relevante,
incluyendo intentos de autenticación fallidos. Nunca registra PIN ni
`session_token` fuera de los modos iniciales que los imprimen para uso manual.

## Integración con Micro Server

Micro Server debe apuntar sus requests HTTP al puerto del simulator en lugar de a la IP del receptor físico. Para discovery, debe configurarse manualmente la IP del simulator (no hay mDNS simulado).

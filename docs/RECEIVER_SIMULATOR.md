# Michi Music Stream — Receiver Simulator

## Propósito

El `simulator/` permite probar la integración de receptores Michi Music Stream con Micro Server sin depender de hardware ESP32 real. Implementa el protocolo v1-lite completo sobre HTTP.

## Arquitectura

```
Michi Micro Server  ──HTTP──>  receiver_sim.py (puerto 8080)
  (integración real)           Simula Standard o Hi-Fi
```

El simulator se comporta como un receptor físico pero sin audio real ni puerto UDP.

## Capacidades

| Capacidad | Estado |
|-----------|--------|
| receiver/info Standard y Hi-Fi | Completo |
| Pairing con botón (ventana 120 s) | Completo |
| Session/start con validación completa | Completo |
| Heartbeat | Completo |
| Volumen 0-100 | Completo |
| Error format with details | Completo |
| Auth Bearer token | Completo |
| mDNS | No simulado |
| Audio UDP real | No simulado (solo acepta el contrato) |
| Opus decoding | No simulado |

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
python3 tests/test_simulator.py
```

20 tests que cubren: info, pairing open/closed/confirm/nonce, sesión válida/duplicada/códec inválido/rate inválido/depth inválido/channels inválido, heartbeat, volumen, clamp, auth.

## Logs

El simulator genera logs con timestamp ISO8601 para cada evento relevante, incluyendo intentos de autenticación fallidos.

## Integración con Micro Server

Micro Server debe apuntar sus requests HTTP al puerto del simulator en lugar de a la IP del receptor físico. Para discovery, debe configurarse manualmente la IP del simulator (no hay mDNS simulado).

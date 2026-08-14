# Receiver Simulator — Integración Continua

## Resumen

El simulator de Michi Music Stream es la herramienta oficial de validación externa para receptores v1-lite. Micro Server lo usa como blanco E2E antes de integrar hardware ESP32 real. Implementa exclusivamente el contrato canónico del bundle `contracts/michi-link/`.

## Comandos de lanzamiento rápido

```bash
# Standard con pairing abierto (puerto 53319)
scripts/run_receiver_sim_standard.sh

# Hi-Fi con pairing abierto (puerto 53319)
scripts/run_receiver_sim_hifi.sh

# Standard con pairing CERRADO (pair/start responde 403)
scripts/run_receiver_sim_pairing_closed.sh
```

El puerto `53319` es el default recomendado para integración con Micro Server.

## Modos por uso

| Escenario | Comando |
|-----------|---------|
| Standard listo para emparejar | `--type standard --pairing-open --port 53319` |
| Hi-Fi listo para emparejar | `--type hifi --pairing-open --port 53319` |
| Standard con ventana cerrada | `--type standard --pairing-closed --port 53319` |
| Sesión ya activa (test continuidad) | `--type standard --active-session --port 53319` |
| Lease ya expirado (test timeout) | `--type standard --fail-heartbeat --port 53319` |

## Ejemplos de pruebas con curl

El contrato completo (cuerpos, estados y esquemas) está en
`contracts/michi-link/`. A continuación, el flujo canónico:

### 1. Info (sin auth)

```bash
curl -s http://localhost:53319/api/v1/server/info | jq .
# → 200, service=michi-stream-standard, roles=["audio_receiver"]
```

### 2. Pairing completo (ventana abierta con --pairing-open)

```bash
# Terminal 1: simulator con ventana abierta
scripts/run_receiver_sim_standard.sh

# Terminal 2: pair/start firmado (vector del bundle) — crea la sesión de pairing
curl -s http://localhost:53319/api/v1/pair/start \
  -H "Content-Type: application/json" \
  -d "$(cat contracts/michi-link/vectors/pairing/pair-start-valid.json)" | jq .
# → 201 + session_id (el PIN de 6 dígitos se muestra en el log local)

# pair/status
curl -s "http://localhost:53319/api/v1/pair/status?session_id=<session_id>" | jq .
# → 200 + status=pending

# pair/confirm con el PIN del log local (PIN del vector: 042731)
curl -s http://localhost:53319/api/v1/pair/confirm \
  -H "Content-Type: application/json" \
  -d "$(cat contracts/michi-link/vectors/pairing/pair-confirm-valid.json)" | jq .
# → 200 + token (emitido por el receptor; expires_in=0)
```

### 3. Pairing sin ventana → 403 esperado

```bash
scripts/run_receiver_sim_pairing_closed.sh
# En otra terminal:
curl -s http://localhost:53319/api/v1/pair/start \
  -H "Content-Type: application/json" \
  -d "$(cat contracts/michi-link/vectors/pairing/pair-start-valid.json)" | jq .
# → 403 FORBIDDEN
```

### 4. Sesión de audio válida

```bash
# Con el token del pairing:
curl -s http://localhost:53319/api/v1/receiver-lite/session \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <pairing_token>" \
  -d "$(cat contracts/michi-link/examples/positive/receiver-session-create.json)" | jq .
# → 201 + session_id, session_token, lease_seconds=30, effective.stream_port
```

### 5. Cuerpo inválido → 400 INVALID_REQUEST

```bash
curl -s http://localhost:53319/api/v1/receiver-lite/session \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <pairing_token>" \
  -d '{"buffer_ms": 49, "codec": "pcm_s16le", "sample_rate": 48000,
       "bit_depth": 16, "channels": 2, "packet_ms": 10, "payload_type": 97,
       "ssrc": 1, "volume": 70, "transport": "rtp_udp"}' | jq .
# → 400, error.code=INVALID_REQUEST, details.field=buffer_ms
```

### 6. Heartbeat y lease

```bash
curl -s http://localhost:53319/api/v1/receiver-lite/heartbeat \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <pairing_token>" \
  -H "X-Michi-Session: <session_token>" \
  -d '{"session_id": "<session_id>", "sequence": 1, "sent_at_ms": 0}' | jq .
# → 200 + lease_seconds=30; replay → 409 CONFLICT
```

### 7. Fin de sesión

```bash
curl -s -X DELETE http://localhost:53319/api/v1/receiver-lite/session \
  -H "Authorization: Bearer <pairing_token>" \
  -H "X-Michi-Session: <session_token>"
# → 204 (sin cuerpo)
```

## Tests automáticos

```bash
scripts/run_tests.sh
```

Ejecuta, en orden: sync del bundle vendorizado, `simulator/tests/test_simulator.py`
(29 tests), `simulator/tests/test_integration_http.py` (18 tests),
`tests/contract/test_contract.py` (13 casos) y
`tests/e2e/test_e2e_micro_stream.py` (7 casos). La suite completa de pytest
(`python3 -m pytest -q`) recoge 86 tests. Todos deben pasar para considerar
el simulator válido.

## Integración con Micro Server

Micro Server debe conectar contra `http://<sim-ip>:53319/api/v1/`.

Para descubrimiento, configurar manualmente la IP del simulator en Micro
Server (no hay mDNS ni multicast simulado). Todos los endpoints protegidos
usan `Authorization: Bearer <token>`; las mutaciones de sesión exigen además
`X-Michi-Session: <session_token>`.

# Receiver Simulator — Integración Continua

## Resumen

El simulator de Michi Music Stream es la herramienta oficial de validación externa para receptores v1-lite. Micro Server lo usa como blanco E2E antes de integrar hardware ESP32 real.

## Comandos de lanzamiento rápido

```bash
# Standard con pairing abierto (puerto 53319)
scripts/run_receiver_sim_standard.sh

# Hi-Fi con pairing abierto (puerto 53319)
scripts/run_receiver_sim_hifi.sh

# Standard con pairing CERRADO (Micro Server debe llamar pair/start)
scripts/run_receiver_sim_pairing_closed.sh
```

El puerto `53319` es el default recomendado para integración con Micro Server.

## Modos por uso

| Escenario | Comando |
|-----------|---------|
| Standard listo para emparejar | `--standard --pairing-open --port 53319` |
| Hi-Fi listo para emparejar | `--hifi --pairing-open --port 53319` |
| Standard esperando pair/start | `--standard --pairing-closed --port 53319` |
| Sesión ya activa (test continuidad) | `--standard --active-session --port 53319` |
| Heartbeat fallido (test timeout) | `--standard --fail-heartbeat --port 53319` |

## Ejemplos de pruebas con curl

### Pairing abierto → confirm exitoso

```bash
# Terminal 1: simulator
scripts/run_receiver_sim_standard.sh

# Terminal 2: curl
curl -s http://localhost:53319/api/v1/receiver/pair/start \
  -H "Content-Type: application/json" \
  -d '{"initiator":"michi_micro_server","initiator_id":"micro_001"}' | jq .

NONCE=$(curl -s http://localhost:53319/api/v1/receiver/pair/start \
  -H "Content-Type: application/json" \
  -d '{"initiator":"michi_micro_server","initiator_id":"micro_001"}' | jq -r .nonce)

curl -s http://localhost:53319/api/v1/receiver/pair/confirm \
  -H "Content-Type: application/json" \
  -d "{\"nonce\":\"$NONCE\",\"initiator_id\":\"micro_001\",\"token\":\"tok_micro_e2e\"}" | jq .
```

### Pairing cerrado → 409 esperado

```bash
scripts/run_receiver_sim_pairing_closed.sh
# En otra terminal:
curl -s http://localhost:53319/api/v1/receiver/pair/confirm \
  -H "Content-Type: application/json" \
  -d '{"nonce":"x","initiator_id":"micro_001","token":"tok_test"}' | jq .
# → 409 pairing_window_closed
```

### Session/start válido

```bash
# Hacer pairing primero, luego:
curl -s http://localhost:53319/api/v1/receiver/session/start \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_e2e" \
  -d '{"session_id":"sess_e2e","codec":"pcm_s16le","sample_rate":48000,"bit_depth":16,"channels":2,"transport":"udp","stream_port":55300,"buffer_ms":250,"volume":70}' | jq .
```

### Codec inválido → 400 unsupported_codec

```bash
curl -s http://localhost:53319/api/v1/receiver/session/start \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_e2e" \
  -d '{"session_id":"sess_bad","codec":"flac","sample_rate":48000,"bit_depth":16,"channels":2,"transport":"udp","stream_port":55300,"buffer_ms":250,"volume":70}' | jq .
# → error.code = "unsupported_codec", details.requested_codec = "flac"
```

### Volumen fuera de rango

```bash
curl -s http://localhost:53319/api/v1/receiver/volume \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_e2e" \
  -d '{"volume":150}' | jq .
# → volume_set: 100 (clamp automático)
```

### Sesión duplicada → 409

```bash
# Iniciar primera sesión, luego la misma de nuevo:
curl -s http://localhost:53319/api/v1/receiver/session/start \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_e2e" \
  -d '{"session_id":"sess_e2e","codec":"pcm_s16le","sample_rate":48000,"bit_depth":16,"channels":2,"transport":"udp","stream_port":55300,"buffer_ms":250,"volume":70}' | jq .
# Segunda vez → 409 session_active
```

### Heartbeat fallido (timeout)

```bash
scripts/run_receiver_sim_standard.sh --fail-heartbeat --port 53319
# El token es "tok_preinit". El heartbeat responderá pero el
# estado interno marca last_heartbeat=0 → timeout simulado.
```

## Tests automáticos

```bash
scripts/run_tests.sh
```

Ejecuta:
1. `simulator/tests/test_simulator.py` (20 tests)
2. `tests/contract/test_contract.py` (15 tests)

Total: 35 tests, todos deben pasar para considerar el simulator válido.

## Integración con Micro Server

Micro Server debe conectar contra `http://<sim-ip>:53319/api/v1/`.

Para descubrimiento, configurar manualmente la IP del simulator en Micro Server (no hay mDNS simulado). Todos los endpoints auth usan `Authorization: Bearer <token>`.

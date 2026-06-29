# Micro Server — Receiver Testing Guide

Esta guía describe cómo probar la integración de Michi Micro Server con receptores Michi Music Stream usando el simulator oficial.

## Puerto estándar

El puerto recomendado para integración con Micro Server es **53319**.

```
http://<sim-ip>:53319/api/v1/
```

## Lanzamiento rápido

```bash
# Standard listo para emparejar (pairing-open)
scripts/run_receiver_sim_standard.sh

# Hi-Fi listo para emparejar
scripts/run_receiver_sim_hifi.sh

# Standard esperando pair/start (pairing-closed)
scripts/run_receiver_sim_pairing_closed.sh

# Sesión ya activa (para probar continuidad)
python3 simulator/receiver_sim.py --type standard --active-session --port 53319

# Heartbeat fallido (para probar timeout)
python3 simulator/receiver_sim.py --type standard --fail-heartbeat --port 53319
```

## Prerrequisitos

- Python 3 con Flask (`pip install flask`)
- `scripts/` en el PATH o ejecutar desde la raíz del repo

## Escenarios de prueba

### 1. Descubrimiento (manual)

El simulator no implementa mDNS. Configurar Micro Server con:
- IP: `127.0.0.1` (o IP de la máquina del simulator)
- Puerto: `53319`
- device_id: `rcv_sim_standard_001`

Endpoint de verificación:
```bash
curl -s http://127.0.0.1:53319/api/v1/receiver/info | jq .
```

### 2. Pairing exitoso (con --pairing-open)

```bash
# Terminal 1
scripts/run_receiver_sim_standard.sh

# Terminal 2
NONCE=$(curl -s http://127.0.0.1:53319/api/v1/receiver/pair/start \
  -H "Content-Type: application/json" \
  -d '{"initiator":"michi_micro_server","initiator_id":"micro_001"}' | jq -r .nonce)

curl -s http://127.0.0.1:53319/api/v1/receiver/pair/confirm \
  -H "Content-Type: application/json" \
  -d "{\"nonce\":\"$NONCE\",\"initiator_id\":\"micro_001\",\"token\":\"tok_micro_e2e\"}" | jq .

# → 200 + paired
```

### 3. Pairing sin ventana (debe fallar)

```bash
scripts/run_receiver_sim_pairing_closed.sh

# Sin pair/start previo:
curl -s http://127.0.0.1:53319/api/v1/receiver/pair/confirm \
  -H "Content-Type: application/json" \
  -d '{"nonce":"x","initiator_id":"micro_001","token":"tok_test"}' | jq .

# → 409 pairing_window_closed
```

### 4. Sesión de audio válida

```bash
# Asumiendo pairing exitoso con token=tok_micro_e2e
curl -s http://127.0.0.1:53319/api/v1/receiver/session/start \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_e2e" \
  -d '{
    "session_id": "sess_e2e_001",
    "codec": "pcm_s16le",
    "sample_rate": 48000,
    "bit_depth": 16,
    "channels": 2,
    "transport": "udp",
    "stream_port": 55300,
    "buffer_ms": 250,
    "volume": 70
  }' | jq .

# → 200 session_started
```

### 5. Códec inválido

```bash
curl -s http://127.0.0.1:53319/api/v1/receiver/session/start \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_e2e" \
  -d '{
    "session_id": "sess_bad",
    "codec": "flac",
    "sample_rate": 48000,
    "bit_depth": 16,
    "channels": 2,
    "transport": "udp",
    "stream_port": 55300,
    "buffer_ms": 250,
    "volume": 70
  }' | jq .

# → 400 unsupported_codec + details.requested_codec = "flac"
```

### 6. Sesión duplicada

```bash
# Iniciar primera sesión, luego repetir el mismo llamada:
curl -s http://127.0.0.1:53319/api/v1/receiver/session/start \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_e2e" \
  -d '{"session_id":"sess_e2e","codec":"pcm_s16le","sample_rate":48000,"bit_depth":16,"channels":2,"transport":"udp","stream_port":55300,"buffer_ms":250,"volume":70}' | jq .

# Segunda vez (sin session/stop) → 409 session_active
```

### 7. Heartbeat

```bash
curl -s http://127.0.0.1:53319/api/v1/receiver/heartbeat \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_e2e" \
  -d '{"session_id": "sess_e2e"}' | jq .

# → 200 + uptime_seconds
```

### 8. Heartbeat fallido (timeout simulado)

```bash
scripts/run_receiver_sim_standard.sh --fail-heartbeat --port 53319
# Token pregenerado: tok_preinit
# Internamente last_heartbeat=0 → timeout simulado en 90s
```

### 9. Volumen

```bash
curl -s http://127.0.0.1:53319/api/v1/receiver/volume \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_e2e" \
  -d '{"volume": 50}' | jq .

# → 200 volume_set
```

### 10. Volumen fuera de rango

```bash
curl -s http://127.0.0.1:53319/api/v1/receiver/volume \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_e2e" \
  -d '{"volume": 150}' | jq .

# → 200 volume_set + volume: 100 (clamp automático)
```

### 11. Fin de sesión

```bash
curl -s http://127.0.0.1:53319/api/v1/receiver/session/stop \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_e2e" \
  -d '{"session_id": "sess_e2e"}' | jq .

# → 200 session_stopped
```

### 12. Auth inválido

```bash
curl -s http://127.0.0.1:53319/api/v1/receiver/volume \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer wrong_token" \
  -d '{"volume": 50}' | jq .

# → 401 invalid_token
```

## Validación de errores

Toda respuesta de error sigue el formato:
```json
{
  "error": {
    "code": "codigo",
    "message": "descripcion",
    "details": {}
  }
}
```

## Checklist de integración

- [ ] Micro Server lanza simulator con `scripts/run_receiver_sim_standard.sh`
- [ ] Micro Server consulta `GET /api/v1/receiver/info` → 200 + schema v1-lite
- [ ] Micro Server hace `pair/start` → 200 + nonce
- [ ] Micro Server hace `pair/confirm` → 200 + paired
- [ ] Micro Server recibe 409 si `pair/confirm` sin ventana
- [ ] Micro Server inicia `session/start` → 200 + session_started
- [ ] Micro Server recibe 400 si codec inválido (`unsupported_codec` con details)
- [ ] Micro Server recibe 400 si sample rate inválido (`unsupported_rate` con details)
- [ ] Micro Server recibe 409 si `session/start` duplicado (`session_active` con details)
- [ ] Micro Server envía heartbeat → 200 + uptime_seconds
- [ ] Micro Server cambia volumen → 200 + volume_set
- [ ] Micro Server recibe volumen clamp (150 → 100)
- [ ] Micro Server detiene sesión → 200 + session_stopped
- [ ] Micro Server recibe 401 si token inválido
- [ ] Todas las respuestas de error tienen formato con `details`


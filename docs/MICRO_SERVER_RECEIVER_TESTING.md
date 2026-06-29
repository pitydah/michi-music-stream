# Micro Server — Receiver Testing Guide

Esta guía describe cómo probar la integración de Michi Micro Server con receptores Michi Music Stream usando el simulator.

## Prerrequisitos

- Python 3 con Flask instalado (`pip install flask`)
- `receiver_sim.py` en `simulator/`
- Acceso a la API de Micro Server (o mock)

## Escenarios de prueba

### 1. Descubrimiento (manual)

El simulator no implementa mDNS. Para simular descubrimiento:

1. Iniciar el simulator:
   ```bash
   cd simulator
   python3 receiver_sim.py --type standard --port 8080
   ```

2. Configurar Micro Server con la IP del simulator como receptor conocido:
   - IP: `127.0.0.1` (o la IP de la máquina)
   - Puerto: `8080`
   - device_id: `rcv_sim_standard_001`

3. Verificar que Micro Server puede consultar `GET http://127.0.0.1:8080/api/v1/receiver/info`

### 2. Pairing exitoso

```bash
# 1. Iniciar ventana (simula botón presionado)
# El simulator tiene ventana cerrada por defecto.
# Micro Server envía pair/start:
curl -X POST http://127.0.0.1:8080/api/v1/receiver/pair/start \
  -H "Content-Type: application/json" \
  -d '{"initiator": "michi_micro_server", "initiator_id": "micro_001"}'

# → 200 + nonce

# 2. Confirmar pairing:
curl -X POST http://127.0.0.1:8080/api/v1/receiver/pair/confirm \
  -H "Content-Type: application/json" \
  -d '{"nonce": "<nonce>", "initiator_id": "micro_001", "token": "tok_micro_abc123"}'

# → 200 + token guardado
```

### 3. Pairing sin ventana (debe fallar)

```bash
# Sin llamar a pair/start primero:
curl -X POST http://127.0.0.1:8080/api/v1/receiver/pair/confirm \
  -H "Content-Type: application/json" \
  -d '{"nonce": "x", "initiator_id": "micro_001", "token": "tok_test"}'

# → 409 pairing_window_closed
```

### 4. Sesión de audio válida

```bash
# 1. Pairing exitoso (ver escenario 2)

# 2. Iniciar sesión:
curl -X POST http://127.0.0.1:8080/api/v1/receiver/session/start \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_abc123" \
  -d '{
    "session_id": "sess_test_001",
    "codec": "pcm_s16le",
    "sample_rate": 48000,
    "bit_depth": 16,
    "channels": 2,
    "transport": "udp",
    "stream_port": 55300,
    "buffer_ms": 250,
    "volume": 70
  }'

# → 200 session_started
```

### 5. Códec inválido

```bash
curl -X POST http://127.0.0.1:8080/api/v1/receiver/session/start \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_abc123" \
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
  }'

# → 400 unsupported_codec con details
```

### 6. Sesión duplicada

```bash
# Iniciar primera sesión (ver escenario 4)
# Iniciar segunda sesión sin detener la primera:
curl -X POST ... /session/start ...

# → 409 session_active con details
```

### 7. Heartbeat

```bash
# Con sesión activa:
curl -X POST http://127.0.0.1:8080/api/v1/receiver/heartbeat \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_abc123" \
  -d '{"session_id": "sess_test_001"}'

# → 200 + uptime_seconds
```

### 8. Volumen

```bash
curl -X POST http://127.0.0.1:8080/api/v1/receiver/volume \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_abc123" \
  -d '{"volume": 50}'

# → 200 volume_set
```

### 9. Fin de sesión

```bash
curl -X POST http://127.0.0.1:8080/api/v1/receiver/session/stop \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer tok_micro_abc123" \
  -d '{"session_id": "sess_test_001"}'

# → 200 session_stopped
```

### 10. Auth inválido

```bash
curl -X POST http://127.0.0.1:8080/api/v1/receiver/volume \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer wrong_token" \
  -d '{"volume": 50}'

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

- [ ] Micro Server descubre receiver (manual por ahora)
- [ ] Micro Server hace pair/start exitoso
- [ ] Micro Server hace pair/confirm exitoso
- [ ] Micro Server recibe 409 si pairing sin ventana
- [ ] Micro Server inicia session/start exitoso
- [ ] Micro Server recibe 400 si codec inválido
- [ ] Micro Server recibe 400 si sample rate inválido
- [ ] Micro Server recibe 409 si sesión duplicada
- [ ] Micro Server envía heartbeat exitoso
- [ ] Micro Server cambia volumen exitoso
- [ ] Micro Server detiene sesión exitoso
- [ ] Micro Server recibe 401 si token inválido
- [ ] Todas las respuestas de error tienen formato con details

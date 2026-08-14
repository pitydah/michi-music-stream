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

# Standard con pairing abierto + PIN visible en el "display local" (SOLO desarrollo)
python3 simulator/receiver_sim.py --type standard --pairing-open --show-local-pairing-pin --port 53319
```

El puerto `53319` es el default recomendado para integración con Micro Server.

## Modos por uso

| Escenario | Comando |
|-----------|---------|
| Standard listo para emparejar | `--type standard --pairing-open --port 53319` |
| Hi-Fi listo para emparejar | `--type hifi --pairing-open --port 53319` |
| Standard con ventana cerrada | `--type standard --pairing-closed --port 53319` |
| Standard con PIN en display local (dev) | `--type standard --pairing-open --show-local-pairing-pin --port 53319` |
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

### 2. Pairing manual con datos dinámicos (ventana abierta con --pairing-open)

El PIN y el session_id son ALEATORIOS en cada ejecución del simulador
(`secrets.randbelow` + `uuid.uuid4` en runtime). Los vectores de pairing
del bundle con PIN fijo (`pair-confirm-*.json`) NO sirven contra el
simulador en runtime: confirmarlos responde `401 PAIRING_PIN_MISMATCH`.
El flujo manual debe leer los valores reales en runtime.

**Canal del PIN**: el PIN solo existe dentro del proceso del simulador y
se entrega por el "display local" (nunca por HTTP, por diseño del
contrato). Para un flujo manual/CI, el simulador ofrece el flag
exclusivamente de desarrollo `--show-local-pairing-pin`:

- por defecto el PIN **no** aparece en logs (solo
  `Pairing session created: <uuid> (PIN displayed locally)`);
- al activarlo imprime una advertencia y, por cada sesión de pairing:
  `[LOCAL DISPLAY] Pairing PIN: 123456` y
  `[LOCAL DISPLAY] Pairing session: <uuid>`;
- es SOLO desarrollo: nunca existe en firmware ni en producción;
- nunca imprime tokens de producción (el Bearer jamás se imprime; el PIN
  sí, únicamente bajo este flag).

```bash
# Terminal 1: simulator con ventana abierta y display local de desarrollo
python3 simulator/receiver_sim.py --type standard --pairing-open --show-local-pairing-pin --port 53319

# Terminal 2: pair/start firmado (vector del bundle) — crea la sesión de pairing
START=$(curl -s http://localhost:53319/api/v1/pair/start \
  -H "Content-Type: application/json" \
  -d "$(cat contracts/michi-link/vectors/pairing/pair-start-valid.json)")
echo "$START" | jq .
# → 201; extraer el session_id DE LA RESPUESTA (nunca de un vector)
SESSION_ID=$(echo "$START" | jq -r .session_id)

# pair/status con la sesión real
curl -s "http://localhost:53319/api/v1/pair/status?session_id=$SESSION_ID" | jq .
# → 200 + status=pending

# Leer en Terminal 1:
#   [LOCAL DISPLAY] Pairing PIN: 042731
#   [LOCAL DISPLAY] Pairing session: <uuid>   ← debe coincidir con $SESSION_ID

# pair/confirm con PIN y sesión REALES (leídos del display local)
# IMPORTANTE: la sesión se consume en esta llamada; el token SOLO está en
# ESTA respuesta (un segundo confirm responde 409 PAIRING_ALREADY_CONSUMED
# y no incluye token). Guardá la respuesta y extraé el token de ella.
CONFIRM_RESPONSE=$(curl -s http://localhost:53319/api/v1/pair/confirm \
  -H "Content-Type: application/json" \
  -d "$(jq -n --arg sid "$SESSION_ID" --arg pin "<PIN_DEL_DISPLAY_LOCAL>" \
       '{session_id: $sid, pin: $pin,
         michi_id: "JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U",
         public_key: "j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E"}')")
echo "$CONFIRM_RESPONSE" | jq .
# → 200 + token (emitido por el receptor; expires_in=0)
TOKEN=$(echo "$CONFIRM_RESPONSE" | jq -r .token)
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
  -H "Authorization: Bearer $TOKEN" \
  -d "$(cat contracts/michi-link/examples/positive/receiver-session-create.json)" | jq .
# → 201 + session_id, session_token, lease_seconds=30, effective.stream_port
```

### 5. Cuerpo inválido → 400 INVALID_REQUEST

```bash
curl -s http://localhost:53319/api/v1/receiver-lite/session \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"buffer_ms": 49, "codec": "pcm_s16le", "sample_rate": 48000,
       "bit_depth": 16, "channels": 2, "packet_ms": 10, "payload_type": 97,
       "ssrc": 1, "volume": 70, "transport": "rtp_udp"}' | jq .
# → 400, error.code=INVALID_REQUEST, details.field=buffer_ms
```

### 6. Heartbeat y lease

```bash
curl -s http://localhost:53319/api/v1/receiver-lite/heartbeat \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -H "X-Michi-Session: <session_token>" \
  -d '{"session_id": "<session_id>", "sequence": 1, "sent_at_ms": 0}' | jq .
# → 200 + lease_seconds=30; replay → 409 CONFLICT
```

### 7. Fin de sesión

```bash
curl -s -X DELETE http://localhost:53319/api/v1/receiver-lite/session \
  -H "Authorization: Bearer $TOKEN" \
  -H "X-Michi-Session: <session_token>"
# → 204 (sin cuerpo)
```

## Tests automáticos

```bash
scripts/run_tests.sh
```

Ejecuta, en orden: sync del bundle vendorizado, `simulator/tests/test_simulator.py`
(29 tests), `simulator/tests/test_integration_http.py` (18 tests),
`tests/contract/test_contract.py` (13 casos),
`tests/contract/test_examples.py` (validación de ejemplos oficiales) y
`tests/e2e/test_e2e_micro_stream.py` (7 casos). La suite completa de pytest
(`python3 -m pytest -q`) recoge también
`tests/contract/test_schema.py` (suites de esquemas del bundle),
`tests/contract/test_examples.py` y el smoke de pairing manual
`tests/e2e/test_manual_pairing_smoke.py`. Todos deben pasar para considerar
el simulator válido.

## Integración con Micro Server

Micro Server debe conectar contra `http://<sim-ip>:53319/api/v1/`.

Para descubrimiento, configurar manualmente la IP del simulator en Micro
Server (no hay mDNS ni multicast simulado). Todos los endpoints protegidos
usan `Authorization: Bearer <token>`; las mutaciones de sesión exigen además
`X-Michi-Session: <session_token>`.

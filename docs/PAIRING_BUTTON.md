# Botón de Pairing

## Propósito

Único mecanismo para abrir la ventana de pairing con un controlador. Ninguna llamada HTTP puede abrirla (fuera de la ventana, `POST /pair/start` responde 403). El mismo botón es la única vía física de recovery y factory reset.

## Gestos determinísticos

Umbrales de Kconfig (componente `michi_button`): `MICHI_BUTTON_RECOVERY_PRESS_MS` (5000 ms), `MICHI_BUTTON_FACTORY_RESET_PRESS_MS` (10000 ms), `MICHI_BUTTON_FACTORY_ARM_MS` (10000 ms). Ambos gestos están siempre compilados (no hay choice que los excluya).

| Gesto | Duración | Acción |
|-------|----------|--------|
| Pulsación corta | < 5000 ms | Abre la ventana de pairing (ver tabla de estados) |
| Pulsación larga | >= 5000 ms y < 10000 ms | Recovery: postea `MICHI_EVENT_RECOVER` SOLO si el estado es `RECOVERABLE_ERROR`; en cualquier otro estado se ignora |
| Pulsación muy larga | >= 10000 ms | Factory reset: borra toda la NVS (identidad, controladores, Wi-Fi, server_id) y reinicia |

Condiciones del factory reset (en código y Kconfig):

- La pulsación comenzó al menos `MICHI_BUTTON_FACTORY_ARM_MS` (10000 ms)
  después del arranque: mantener el botón durante el encendido NUNCA
  dispara el reset.
- La pulsación no comenzó ni terminó en `BOOTING`, `SELF_TEST` ni
  `UPDATING` (bloqueado durante OTA).
- El recovery NO está armado: funciona apenas se confirma la pulsación
  (>= 5 s) con el estado `RECOVERABLE_ERROR`.

## Estados

| Estado | Descripción |
|--------|-------------|
| `unpaired` | Sin controlador asociado |
| `paired` | Con controlador(es) asociado(s) |
| `pairing_window_open` | Ventana de 120 s abierta (reloj monotónico) |
| `factory_reset` | Pulsación muy larga >= 10 s (con arm window) |

## Pulsación corta (< 5 s)

| Estado actual | Comportamiento |
|---------------|---------------|
| `unpaired` | Abre ventana 120 s |
| `paired` | Abre ventana 120 s para un nuevo controlador |
| `pairing_window_open` | Reemplaza la ventana previa y elimina sesiones de pairing pendientes |

## Pulsación larga (5 s - 10 s)

Recovery (`MICHI_EVENT_RECOVER`): solo si el estado es `RECOVERABLE_ERROR`.
En cualquier otro estado la pulsación se ignora (si el dispositivo ya se
recuperó solo, no hace nada).

## Pulsación muy larga (>= 10 s)

Factory reset: borra toda la NVS (incluida la identidad y los
controladores), limpia la identidad y el registro de controladores en RAM,
detiene la sesión reiniciando y reinicia. Sujeto a la arm window y a los
estados protegidos (ver arriba).

## Identidad corrupta

Una identidad corrupta exige factory reset, pero NO bloquea el gesto: la
corrupción de identidad no cambia el estado de la FSM (el dispositivo
sigue corriendo), así que la pulsación >= 10 s sigue disponible y es la
ÚNICA vía física de recuperación. El reset borra el store corrupto y el
próximo arranque genera una identidad nueva.

## Ventana de pairing

- 120 segundos desde que se presiona el botón; reiniciar el dispositivo la cierra.
- Solo durante la ventana `pair/start` crea una sesión de pairing.
- El receptor muestra un PIN criptográfico de 6 dígitos SOLO en la pantalla
  local (nunca por HTTP).
- Máximo 5 intentos fallidos de PIN por sesión; después → 429 y la sesión
  queda consumida.
- La confirmación exitosa consume la sesión (segunda confirmación → 409).

## Almacenamiento (NVS)

- Identidad Ed25519 persistente (seed + clave).
- Controladores: `michi_id`, `public_key`, digest SHA-256 del token,
  permisos, fecha de creación y última actividad. Nunca el PIN ni el token
  en claro.
- Factory reset borra todo; una identidad corrupta exige factory reset
  (accesible físicamente con la pulsación >= 10 s).

## LED

| Estado | LED |
|--------|-----|
| Unpaired | Azul fijo |
| Pairing window open | Barrido azul |
| Paired, sin sesión | Azul fijo |
| Sesión activa | Verde fijo |
| Error / sin Wi-Fi | Rojo fijo |
| Factory reset | Rojo intermitente (100 ms) |

## Seguridad

- No se acepta pairing remoto sin botón físico presionado.
- `pair/start` exige challenge firmado (Ed25519) y que `michi_id` corresponda
  a la `public_key` declarada; un fallo responde 400 y no crea sesión.
- El token lo genera el receptor con 32 bytes CSPRNG (base64url sin padding),
  se entrega una sola vez y solo se persiste su digest SHA-256.
- Comparaciones en tiempo constante; cero secretos en logs.

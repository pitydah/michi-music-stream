# Botón de Pairing

## Propósito

Único mecanismo para abrir la ventana de pairing con un controlador. Ninguna llamada HTTP puede abrirla (fuera de la ventana, `POST /pair/start` responde 403).

## Estados

| Estado | Descripción |
|--------|-------------|
| `unpaired` | Sin controlador asociado |
| `paired` | Con controlador(es) asociado(s) |
| `pairing_window_open` | Ventana de 120 s abierta (reloj monotónico) |
| `factory_reset` | Pulsación larga >10 s |

## Pulsación corta (50 ms - 3 s)

| Estado actual | Comportamiento |
|---------------|---------------|
| `unpaired` | Abre ventana 120 s |
| `paired` | Abre ventana 120 s para un nuevo controlador |
| `pairing_window_open` | Reemplaza la ventana previa y elimina sesiones de pairing pendientes |

## Pulsación larga (>10 s)

Factory reset: borra toda la NVS (incluida la identidad y los controladores), detiene sesión, reinicia.

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
- Factory reset borra todo; una identidad corrupta exige factory reset.

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

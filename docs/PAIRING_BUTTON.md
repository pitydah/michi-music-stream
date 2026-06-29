# Botón de Pairing

## Propósito

Único mecanismo para iniciar emparejamiento con un controlador.

## Estados

| Estado | Descripción |
|--------|-------------|
| `unpaired` | Sin controlador asociado |
| `paired` | Con controlador asociado |
| `pairing_window_open` | Ventana de 120 s abierta |
| `factory_reset` | Pulsación larga >10 s |

## Pulsación corta (50 ms - 3 s)

| Estado actual | Comportamiento |
|---------------|---------------|
| `unpaired` | Abre ventana 120 s |
| `paired` | Abre ventana 120 s para nuevo controlador |
| `pairing_window_open` | Reinicia temporizador a 120 s |

## Pulsación larga (>10 s)

Factory reset: borra todos los tokens NVS, detiene sesión, reinicia.

## Ventana de pairing

- 120 segundos desde que se presiona el botón.
- Solo durante la ventana se acepta `pair/start` y `pair/confirm`.
- Al expirar sin confirmación → ventana se cierra.

## Almacenamiento

- Hasta 4 controladores en NVS.
- Cada entrada: `controller_id` + `token`.
- Factory reset borra todo.

## LED

| Estado | LED |
|--------|-----|
| Unpaired | Azul fijo |
| Pairing window open | Amarillo intermitente (500 ms) |
| Paired, sin sesión | Azul fijo |
| Sesión activa | Verde fijo |
| Error / sin Wi-Fi | Rojo fijo |
| Factory reset | Rojo intermitente (100 ms) |

## Seguridad

No se acepta pairing remoto sin botón físico presionado.

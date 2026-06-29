# Sesión de Audio

## Ciclo de vida

```
session/start ──► heartbeat (cada 30 s) ──► session/stop
                       │
                       └──► timeout 90 s ──► auto-stop
```

## Inicio

El controlador envía `session/start` con parámetros de audio.

| Parámetro | Standard | Hi-Fi |
|-----------|----------|-------|
| codec | pcm_s16le, opus | pcm_s16le, pcm_s24le, opus |
| sample_rate máx | 48000 | 96000 |
| bit_depth máx | 16 | 24 |
| buffer_ms típico | 250 | 500 |
| volume | 0-100 | 0-100 |

## Buffer de jitter

```
Paquetes UDP ──► Buffer circular ──► DAC I2S
                 [buffer_ms]
```

- Se llena durante `buffer_ms` antes de empezar reproducción.
- Underrun: silencio.
- Overrun: descartar paquetes viejos.

## Heartbeat

- Intervalo: 30 s.
- Timeout: 90 s sin heartbeat → sesión detenida.

## Volumen

Entero 0-100. Ganancia digital antes del DAC.

## Sesión única

Solo una sesión activa a la vez. `session/start` con sesión activa → 409.

## Tasa de datos

| Config | Tasa | Paq/s | Buffer RAM |
|--------|------|-------|------------|
| 48k/16b/stéreo | 1.5 Mbps | 100 | ~48 KB |
| 96k/24b/stéreo | 4.6 Mbps | 100 | ~288 KB |

# Michi Music Stream Standard — Hardware

## Perfil

| Atributo | Valor |
|----------|-------|
| Código | `michi_stream_standard` |
| Costo | Bajo |
| MCU | ESP32-S3 (Xtensa LX7) |
| Salida | Jack 3.5 mm estéreo |
| Audio máx. | PCM 16-bit / 48 kHz / 2 canales |
| Codecs | `pcm_s16le` (certificado hoy; Opus es futuro, no implementado) |

## Componentes sugeridos

| Componente | Recomendación | Alternativa |
|------------|---------------|-------------|
| MCU | ESP32-S3-WROOM-1-N8R2 | ESP32-C3 |
| DAC I2S | PCM5102A (SNR 112 dB, propuesto) | MAX98357A (DAC+amp), ES9023, AK4432 |
| Regulador | AMS1117-3.3 | ME6211 |
| Botón pairing | Pulsador táctil SMD 6×6 mm | — |
| LED estado | WS2812B RGB | LED bicolor |

## Diagrama

```
┌──────────────┐   I2S    ┌──────────────┐   ┌──────────────┐
│   ESP32-S3   │─BCLK────>│              │   │              │
│              │─LRC─────>│   PCM5102A   │──>│  Jack 3.5 mm │
│              │─DIN─────>│   (DAC I2S)  │   │  (salida)    │
└──────────────┘          └──────────────┘   └──────────────┘
```

## Pines I2S (Kconfig Authoritative)

| Señal | GPIO | Nota |
|-------|------|------|
| BCLK | 3 | I2S bit clock (strapping pin ESP32-S3) |
| LRCK | 18 | I2S left/right frame clock |
| DIN | 5 | I2S serial audio data |
| MCLK | -1 | No conectado / no requerido (PCM5102A PLL genera reloj interno desde BCK) |

## GPIO

| Pin | Función |
|-----|---------|
| 17 | Botón pairing (input pull-up activo en bajo, flanco descendente) |
| 4 | LED estado (SK6812 / WS2812B) |
| 3 | I2S BCLK |
| 18 | I2S LRCK |
| 5 | I2S DIN |

## LED

| Estado | Color |
|--------|-------|
| Wi-Fi OK, sin sesión | Azul fijo |
| Sesión activa | Verde fijo |
| Pairing abierto | Amarillo intermitente (500 ms) |
| Error / sin Wi-Fi | Rojo fijo |
| Factory reset | Rojo intermitente (100 ms) |

## Alimentación

USB-C 5 V / 150 mA estimado.

> **Nota:** PCM5102A es una propuesta inicial. No bloquea cambios futuros a ES9023,
> AK4432 u otro DAC I2S de 16-bit. La elección final depende de disponibilidad,
> costo y validación acústica con el parlante o equipo auxiliar objetivo.

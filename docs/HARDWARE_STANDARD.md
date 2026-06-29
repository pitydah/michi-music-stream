# Michi Music Stream Standard — Hardware

## Perfil

| Atributo | Valor |
|----------|-------|
| Código | `michi_stream_standard` |
| Costo | Bajo |
| MCU | ESP32-S3 (Xtensa LX7) |
| Salida | Jack 3.5 mm estéreo |
| Audio máx. | PCM 16-bit / 48 kHz / 2 canales |
| Codecs | `pcm_s16le`, `opus` |

## Componentes sugeridos

| Componente | Recomendación | Alternativa |
|------------|---------------|-------------|
| MCU | ESP32-S3-WROOM-1-N8R2 | ESP32-C3 |
| DAC I2S | PCM5102A (SNR 112 dB) | MAX98357A (DAC+amp) |
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

## Pines I2S

| Señal | GPIO |
|-------|------|
| BCLK | 6 |
| LRC | 7 |
| DIN | 8 |

## GPIO

| Pin | Función |
|-----|---------|
| 3 | Botón pairing (input pull-up, flanco descendente) |
| 4 | LED WS2812B |
| 6 | I2S BCLK |
| 7 | I2S LRC |
| 8 | I2S DIN |

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

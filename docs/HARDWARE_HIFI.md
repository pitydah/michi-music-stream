# Michi Music Stream Hi-Fi — Hardware

## Perfil

| Atributo | Valor |
|----------|-------|
| Código | `michi_stream_hifi` |
| Costo | Medio |
| MCU | ESP32-S3 |
| DAC | PCM5122 (TI, SNR 112 dB, 384 kHz máx) |
| Buffer salida | NE5532 (opamp doble) |
| Salida | RCA estéreo (2 × hembra dorada) |
| Audio máx. | PCM 24-bit / 96 kHz / 2 canales |
| Codecs | `pcm_s16le`, `pcm_s24le`, `opus` |
| OTA | Sí |

## Componentes

| Componente | Recomendación |
|------------|---------------|
| MCU | ESP32-S3-WROOM-1-N16R8 |
| DAC | PCM5122 (I2S + I2C control) |
| Buffer | NE5532 |
| Reg. digital | LM1117-3.3 |
| Reg. analógico | LT1963A-3.3 (bajo ruido) |
| Reloj | Cristal 24.576 MHz |
| Antena | U.FL + externa |

## Diagrama

```
┌──────────────┐  I2S    ┌──────────────┐   ┌──────────────┐
│   ESP32-S3   │─MCLK───>│              │   │              │
│              │─BCLK───>│   PCM5122    │──>│  NE5532      │──> RCA L
│              │─LRC────>│  (DAC Hi-Fi) │   │  (buffer)    │
│              │─DIN────>│              │   │              │──> RCA R
└──────────────┘         └──────────────┘   └──────────────┘
```

## Pines I2S

| Señal | GPIO | PCM5122 pin |
|-------|------|-------------|
| MCLK | 9 | 12 (SCK) |
| BCLK | 6 | 13 (BCK) |
| LRC | 7 | 15 (LRCK) |
| DIN | 8 | 14 (DATA) |

## I2C (PCM5122)

| Señal | GPIO | Dirección |
|-------|------|-----------|
| SDA | 1 | `0x4D` |
| SCL | 2 | — |

## Alimentación

USB-C 5 V / 1 A.
Reguladores separados para sección digital y analógica.
Star ground. Ferrita en fuente.

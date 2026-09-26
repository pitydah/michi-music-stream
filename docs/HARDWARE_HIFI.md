# Michi Music Stream Hi-Fi — Hardware

## Perfil

| Atributo | Valor |
|----------|-------|
| Código | `michi_stream_hifi` |
| Costo | Medio |
| MCU | ESP32-S3 |
| DAC | PCM5122 (TI, SNR 112 dB, 384 kHz máx en silicio; 48 kHz / 16-bit validado en firmware) |
| Buffer salida | Integrado en PCM5122 (DirectPath™ ground-centered, 2.1 Vrms, salida analógica directa) |
| Salida | RCA estéreo (2 × hembra dorada) |
| Audio máx. | PCM 16-bit / 48 kHz / 2 canales (certificado hoy) |
| Codecs | `pcm_s16le` (certificado hoy) |
| OTA | Sí |

## Componentes propuestos

| Componente | Propuesta | Alternativas |
|------------|-----------|--------------|
| MCU | ESP32-S3-WROOM-1-N16R8 | ESP32-P4 |
| DAC | PCM5122 (I2S + I2C) | AK4432, ES9023, PCM5242 |
| Buffer | Integrado (DirectPath™ salida directa) | NE5532, OPA2134 (filtro LP activo de 3er orden opcional) |
| Reg. digital | LM1117-3.3 | — |
| Reg. analógico | LT1963A-3.3 (bajo ruido) | ADP150, TPS7A47 |
| Reloj | PLL interno desde BCK (MCLK=-1) | Cristal / oscilador 24.576 MHz dedicado (modo maestro síncrono futuro) |
| Antena | U.FL + externa | PCB trace |

> **Advertencia importante:**
> El diseño Hi-Fi requiere validación de:
> - Ruido de fuente: plano de tierra separado, star ground, ferrita en alimentación.
> - Aislamiento I2S: rutas digitales cerca de pistas analógicas pueden inducir ruido.
> - Estabilidad del regulador analógico: LT1963A requiere capacitores de salida
>   específicos (10 µF tantalio o 22 µF cerámico).
>
> No considerar esta BOM como definitiva sin pruebas en PCB real.
>
> **Capacidades del silicio vs anunciadas en firmware (Sección 18):** el silicio del
> PCM5122 admite hasta PCM 32-bit / 384 kHz (y SNR 112 dB), pero la capacidad
> validada y certificada anunciada por el firmware hoy es estrictamente `pcm_s16le`
> a 48 kHz / 16 bit / 2 canales. El endpoint `/diagnostics` refleja `sample_rate: 48000`
> y `bit_depth: 16` como baseline activo del perfil, mientras que las capacidades máximas
> de silicio pertenecen al hardware subyacente. S24LE, S32LE y Opus son trabajo futuro
> (ver `docs/ROADMAP.md`); no hay camino implementado ni anunciado hoy.

## Diagrama

```
┌──────────────┐  3-wire I2S  ┌────────────────────────┐
│   ESP32-S3   │─BCLK (3)────>│                        │
│              │─LRCK (18)───>│        PCM5122         │──> RCA L (Single-ended)
│              │─DIN (5)─────>│    (PLL from BCK,      │
│              │              │  internal line driver) │──> RCA R (Single-ended)
│              │─SDA (21)────>│                        │
│              │─SCL (16)────>│                        │
└──────────────┘  I2C (0x4C/4D)└────────────────────────┘
```

## Pines I2S (Kconfig Authoritative)

| Señal | GPIO | PCM5122 pin | Nota |
|-------|------|-------------|------|
| MCLK | -1 | SCK (12) / GND | No requerido: PCM5122 PLL genera reloj interno desde BCK |
| BCLK | 3 | 13 (BCK) | I2S bit clock (strapping pin ESP32-S3) |
| LRCK | 18 | 15 (LRCK) | I2S left/right frame clock |
| DIN | 5 | 14 (DIN) | I2S serial audio data |

## I2C Control Bus (PCM5122)

| Señal | GPIO | Dirección I2C | Nota |
|-------|------|---------------|------|
| SDA | 21 | `0x4C` / `0x4D` | Bus 100 kHz (pull-ups externos 2.2-4.7 kΩ recomendados) |
| SCL | 16 | — | Bus 100 kHz |

## Salida de Audio y Alimentación

- **Topología de salida:** Single-ended estéreo centrada en masa (2.1 Vrms típica a 3.3 V AVDD). El PCM5122 integra buffers de línea internos con bomba de carga, eliminando condensadores de acoplo DC y op-amps intermedios (como el NE5532) para ruta analógica directa de bajo ruido.
- **Alimentación:** USB-C 5 V / 1 A mínimo con filtrado por ferrita y reguladores LDO de ultra bajo ruido dedicados para riel analógico (AVDD 3.3 V) y digital (DVDD 3.3 V).

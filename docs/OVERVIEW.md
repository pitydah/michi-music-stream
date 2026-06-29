# Michi Music Stream — Overview

## Qué es

Michi Music Stream es una familia de receptores físicos de audio diseñados para integrarse al ecosistema Michi. Su única función es recibir un flujo de audio por red y reproducirlo a través de una salida física (jack 3.5 mm o RCA).

## Qué no es

- No es una biblioteca musical.
- No administra playlists.
- No indexa música.
- No es un servidor musical.
- No tiene almacenamiento local de música.
- No reproduce de forma autónoma.

## Cómo se integra

Michi Music Stream es una salida de audio para el ecosistema. Para funcionar necesita:
1. Una red Wi-Fi local.
2. Un controlador emparejado (Michi Music Player o Michi Micro Server) que envíe comandos REST.
3. Una fuente de audio (Player o Micro Server) que transmita audio por UDP.

## Variantes

| Aspecto | Standard | Hi-Fi |
|---------|----------|-------|
| MCU | ESP32-S3 | ESP32-S3 |
| DAC | I2S básico (PCM5102A) | DAC Hi-Fi (PCM5122) |
| Salida | Jack 3.5 mm | RCA estéreo |
| Audio máx. | PCM 16-bit / 48 kHz | PCM 24-bit / 96 kHz |
| Codecs | pcm_s16le, opus | pcm_s16le, pcm_s24le, opus |
| OTA | No | Sí |

## Protocolo: Michi Link v1-lite

Subconjunto REST liviano sobre HTTP con audio por UDP.

Volumen como entero **0-100** para compatibilidad con Player y Mobile.

Formato de error estándar:
```json
{
  "error": {
    "code": "codigo_maquina",
    "message": "Descripción legible",
    "details": {}
  }
}
```

## Seguridad

- Token generado durante pairing, almacenado en NVS.
- Header `Authorization: Bearer <token>` en toda petición POST (excepto pair/start y pair/confirm).
- Factory reset: botón presionado 10 segundos.

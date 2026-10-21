# Michi Music Stream — Arquitectura vigente

## Propósito

Receptor de audio físico para el ecosistema Michi. No indexa música, no aloja bibliotecas ni actúa como servidor. Micro Server transmite audio y Stream lo reproduce por I2S/DAC.

## Flujo de incorporación (sin botón de pairing)

1. Ecosystem aprovisiona Wi-Fi, identidad criptográfica y certificado de membresía firmado por Michi Home en el flujo de flasheo.
2. Al arrancar, Stream importa su configuración de fábrica y conecta a Wi-Fi como estación.
3. Publica `_michi-link._tcp.local` y prepara anuncios firmados por multicast `224.0.0.167:53318`.
4. Micro Server consulta `GET /api/v1/server/info` y completa la autenticación mutuamente verificada mediante `POST /api/v1/auth/challenge` y `POST /api/v1/auth/session`.
5. Una sesión autorizada negocia el transporte RTP por `/api/v1/receiver-lite/session`.

## API efectiva

| Ruta | Método | Rol |
|---|---|---|
| `/api/v1/server/info` | GET | Perfil y capacidades |
| `/api/v1/auth/challenge` | POST | Desafío de autenticación |
| `/api/v1/auth/session` | POST | Prueba de membresía y token |
| `/api/v1/home/revocations` | GET/POST | Revocaciones firmadas por la autoridad Home |
| `/api/v1/receiver-lite/session` | POST/GET/PATCH/DELETE | Ciclo de sesión RTP autorizada |
| `/api/v1/receiver-lite/heartbeat` | POST | Mantener lease |
| `/api/v1/receiver-lite/diagnostics` | GET | Diagnósticos existentes |
| `/api/v1/receiver-lite/now-playing` | PUT | Handler local experimental; feature no anunciado hasta certificación (ver `docs/NOW_PLAYING_EXTENSION.md`) |
| `/api/v1/receiver-lite/firmware` | GET/POST | No asumir OTA HTTP disponible |

**No existen rutas `/api/v1/pair/*` en la tabla vigente.**

## Audio realmente anunciado

Ambas variantes usan PCM estéreo `pcm_s16le`, 48 kHz, 16 bit, PT RTP 97, 10 ms y 1920 bytes de payload. Opus y Hi-Res superior son futuras capacidades; no anunciarlas antes de implementarlas y medirlas en hardware.

## Estado de certificación

Las pruebas de contrato/simulador son distintas de la verificación con placa física y DAC. La validación de escucha, continuidad de sesión, I2S/PCM5122, reloj y reconexión real requiere un procedimiento DEVICE_E2E independiente.

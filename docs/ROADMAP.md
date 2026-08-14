# Michi Music Stream — Roadmap

## Entregado (convergencia v1-lite)

- Contrato canónico de Michi Link vendorizado (`contracts/michi-link/`) con
  check de sync en CI.
- Simulador canónico (MS-02): rutas, estados y errores exactos; validación
  contra el bundle.
- Firmware conforme: HTTP/JSON/errores canónicos (MS-03), identidad
  Ed25519 persistente (MS-04), discovery mDNS + multicast firmado (MS-05),
  pairing RECEIVER_BUTTON (MS-06), ciclo de sesión RTP (MS-07), heartbeat +
  lease y capacidades veraces (MS-08).
- E2E cruzado reproducible (MS-09): `MOCK_PASS` con commits de ambos repos.
- Limpieza del dialecto legacy (MS-10): árboles `firmware/common|standard|hifi`
  y rutas `/api/v1/receiver/*` retirados.

## Futuro — NO implementado

| Capacidad | Estado |
|-----------|--------|
| PCM 24-bit / 96 kHz | Futuro: requiere schema, vector, motor y evidencia física |
| Opus | Futuro: no implementado ni anunciado |
| Multiroom / sincronización | Futuro: fuera del alcance v1-lite |
| RTCP / corrección de drift | Futuro: declarado en el motor de audio |
| TLS / PAKE / Secure Boot / Flash Encryption | Futuro: seguridad de producción (MS-12) |
| OTA vía HTTP canónico | Diferido: `POST /receiver-lite/firmware` responde 501; el componente michi_ota existe para OTA local por SD |

## No planeado

Biblioteca musical, playlists, indexación, reproducción autónoma, Mobile como fuente directa, Bluetooth/AirPlay/Spotify.

## Próximos pasos

- MS-11: validación en hardware (matriz física, `DEVICE_E2E_PASS`).
- MS-12: seguridad de producción (requiere decisiones humanas).

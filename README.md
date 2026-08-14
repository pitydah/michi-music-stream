# Michi Music Stream

Familia de receptores físicos de audio para el ecosistema Michi.

**No es** biblioteca musical, ni administra playlists, ni indexa música, ni funciona como servidor.

**Es** una salida física de audio que recibe flujo desde Michi Micro Server y lo reproduce por jack 3.5 mm o RCA, controlable exclusivamente con el contrato `v1-lite` publicado por Michi Link (bundle vendorizado en `contracts/michi-link/`).

## Arquitectura

```
Mobile controla.
Micro Server transmite.
Michi Stream reproduce.
```

## Productos

| Variante | Salida | Audio certificado | Uso |
|----------|--------|-------------------|-----|
| Standard | Jack 3.5 mm | PCM 16-bit / 48 kHz / estéreo | Cocina, dormitorio, auxiliar |
| Hi-Fi | RCA estéreo | PCM 16-bit / 48 kHz / estéreo | Living, amplificador, Hi-Fi |

> **Nota honesta**: Standard y Hi-Fi anuncian hoy exactamente el mismo audio
> certificado (`pcm_s16le` 48 kHz / 16 bit / 2 canales). Capacidades futuras
> (PCM 24-bit, 96 kHz, Opus) **no están implementadas** ni anunciadas — ver
> `docs/ROADMAP.md`.

## Protocolo: Michi Link v1-lite

REST HTTP en puerto 80 + audio por RTP/UDP (payload 97). Discovery via mDNS
`_michi-link._tcp` + multicast firmado UDP `224.0.0.167:53318`.

| Método | Ruta exacta | Auth | Descripción |
|--------|-------------|------|-------------|
| GET | `/api/v1/server/info` | No | Perfil canónico del receptor |
| POST | `/api/v1/pair/start` | No; ventana física abierta | Inicia sesión de pairing (PIN local) |
| GET | `/api/v1/pair/status` | No; `session_id` query | Estado de la sesión de pairing |
| POST | `/api/v1/pair/confirm` | No; sesión de pairing | Confirma PIN; el receptor emite el token |
| POST | `/api/v1/receiver-lite/session` | Bearer | Inicia la sesión RTP única (201) |
| GET | `/api/v1/receiver-lite/session` | Bearer | Estado de la sesión (sin secretos) |
| PATCH | `/api/v1/receiver-lite/session` | Bearer + sesión | `volume` / `paused` |
| DELETE | `/api/v1/receiver-lite/session` | Bearer + sesión | Cierra la sesión (204) |
| POST | `/api/v1/receiver-lite/heartbeat` | Bearer + sesión | Renueva el lease de 30 s |

Extensiones opcionales: `PUT /api/v1/receiver-lite/now-playing`,
`GET /api/v1/receiver-lite/diagnostics`,
`GET/POST /api/v1/receiver-lite/firmware`. Las rutas del dialecto legacy
(`/api/v1/receiver/*`, `/api/v1/receiver-lite/volume`, etc.) fueron
**retiradas** y responden 404 con error canónico.

El contrato completo (schemas, ejemplos y vectores) vive en
`contracts/michi-link/`; la firma de identidad sigue los golden vectors de
Michi Link.

## Flujo

1. Receptor enciende → Wi-Fi → anuncia mDNS `_michi-link._tcp` + multicast firmado.
2. Micro Server descubre el receptor y consulta `GET /api/v1/server/info`.
3. Usuario presiona el botón físico de pairing (ventana 120 s).
4. Micro Server: `pair/start` → PIN de 6 dígitos aparece en la pantalla local →
   `pair/confirm` con el PIN → el **receptor** genera el token (32 bytes CSPRNG,
   base64url sin padding; solo se persiste su digest SHA-256 en NVS).
5. Micro Server: `POST /receiver-lite/session` → el receptor elige un puerto UDP
   libre (49152–65535) y devuelve `session_token` (solo RAM, una vez).
6. Micro Server transmite PCM S16LE 48 kHz estéreo por RTP/UDP (PT 97, 10 ms =
   1920 bytes por paquete); el receptor valida IP de origen, PT y SSRC.
7. Heartbeat cada 10 s renueva el lease; a los 30 s sin heartbeat la sesión se
   cierra sola.

## Estructura

```
michi-music-stream/
├── README.md
├── LICENSE
├── docs/             # Documentación técnica
├── firmware/         # Código fuente ESP-IDF (universal)
│   ├── main/         # app_main + Kconfig
│   └── components/   # Componentes michi_* (HTTP, pairing, session, audio…)
├── contracts/        # Bundle vendorizado de Michi Link (v1-lite)
├── simulator/        # Simulador canónico del receptor (Flask)
├── examples/         # Payloads JSON de referencia
└── tests/            # contract/, e2e/ y host/ (tests C de fuentes reales)
```

El firmware es **universal**: Standard y Hi-Fi se derivan del perfil de
producto detectado en hardware, no de árboles de build separados.

## Dependencias del tooling (host, P2-01)

El tooling Python (simulador, tests de contrato, E2E) se instala desde el
**lock** `requirements.txt` — es lo que CI ejecuta con
`pip install -r requirements.txt`. Los pins son EXACTOS (`==`), directos y
transitivos, para builds reproducibles.

- `requirements.in`: dependencias directas pineadas — el único archivo que
  se edita a mano.
- `requirements.txt`: lock completo generado desde `requirements.in`
  (regenerar con: `python3 -m venv /tmp/michi-lock && /tmp/michi-lock/bin/pip install -r requirements.in && /tmp/michi-lock/bin/pip freeze | sort > requirements.txt`).
- Actualización deliberada: subir el pin en `requirements.in`, regenerar el
  lock, correr `./scripts/run_tests.sh` y commitear AMBOS archivos juntos.
- Licencias de los deps directos (BSD-3-Clause Flask, MIT pytest/jsonschema,
  CC0-1.0 OR Apache-2.0 blake3, Apache-2.0 OR BSD-3-Clause cryptography):
  ver el encabezado de `requirements.txt`.

Los tests C (`tests/host/`) compilan las fuentes REALES del firmware y
además requieren **libcjson-dev** (Ubuntu/Debian: `sudo apt-get install -y
libcjson-dev`; Arch: `sudo pacman -S cjson`; o un prefix local exportando
`PKG_CONFIG_PATH=<prefix>/lib/pkgconfig`). El runner detecta su ausencia y
falla con el comando de instalación — nunca omite silenciosamente.

### Runner local

`./scripts/run_tests.sh` corre TODA la suite (default, `--full`): sync del
bundle, tests de contrato/schema/ejemplos, simulador (unit/scenarios/HTTP),
E2E completo + reporte determinístico con drift guard, tests C del host y
los dos scans de hardening (referencias legacy y el gate de hardware de
MS-11). `--quick` corre solo las suites Python rápidas + los scans.
Fail-fast con el nombre exacto de la suite que falla y su código de salida
real; la salida nunca se filtra y el resumen final solo lista las suites
ejecutadas.


# Michi Music Stream

> **H22 SOURCE HANDOFF (2026-10-20):** use `SOURCE_BASELINE_H22.md` for the single
> clean source distribution. This is PRETEST, not a built, flashed or hardware-
> certified release. H16R remains the firmware engine baseline; H18-H21 changes
> are incorporated as source. Do not apply historical patch ZIP files.

> **Baseline de trabajo en Google Drive (19 de octubre de 2026): firmware H16R**
> con harness RTP H18 y correcciones de diagnóstico/seguridad H19.
> H19 registra PCM enviado a la cola I2S sin confundirlo con salida audible,
> protege copias temporales de contraseñas Wi-Fi y añade una política opcional
> de WPA2 mínimo. Ver `docs/CURRENT_PRETEST_BASELINE.md` y
> `docs/H19_SOURCE_ENGINEERING_HANDOFF.md`.
> **No se ha ejecutado la suite ni compilado ESP-IDF en esta entrega.**
> El desarrollo preparado en Drive y la validación local son fases separadas.


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

| Variante | Salida | Perfil PCM anunciado | Uso |
|----------|--------|-------------------|-----|
| Standard | Jack 3.5 mm | PCM 16-bit / 48 kHz / estéreo | Cocina, dormitorio, auxiliar |
| Hi-Fi | RCA estéreo | PCM 16-bit / 48 kHz / estéreo | Living, amplificador, Hi-Fi |

> **Nota de compatibilidad**: Standard y Hi-Fi anuncian hoy exactamente el mismo perfil PCM
> implementado (`pcm_s16le` 48 kHz / 16 bit / 2 canales). Capacidades futuras
> (PCM 24-bit, 96 kHz, Opus) **no están implementadas** ni anunciadas — ver
> `docs/ROADMAP.md`.

## Protocolo vigente: Michi Link v1-lite / HOME_MEMBERSHIP

Control REST/HTTP en la LAN; audio PCM por RTP/UDP. Descubrimiento mDNS
`_michi-link._tcp.local` y multicast firmado UDP `224.0.0.167:53318`.

| Método | Ruta | Descripción |
|---|---|---|
| GET | `/api/v1/server/info` | Perfil y capacidades veraces del receptor |
| POST | `/api/v1/auth/challenge` | Desafío criptográfico de dispositivo |
| POST | `/api/v1/auth/session` | Verificación de membresía Michi Home y emisión de Bearer temporal |
| GET/POST | `/api/v1/home/revocations` | Consulta y recepción de revocaciones firmadas por autoridad Home |
| POST/GET/PATCH/DELETE | `/api/v1/receiver-lite/session` | Ciclo de sesión RTP y control de reproducción |
| POST | `/api/v1/receiver-lite/heartbeat` | Renovación del lease de 30 s |
| GET | `/api/v1/receiver-lite/diagnostics` | Diagnóstico del receptor |
| PUT | `/api/v1/receiver-lite/now-playing` | Implementación local experimental para pruebas; feature `now_playing=false` hasta validar con Micro Server (ver `docs/NOW_PLAYING_EXTENSION.md`) |
| GET/POST | `/api/v1/receiver-lite/firmware` | No asumir OTA HTTP funcional |

**No hay botón de pairing ni rutas `/api/v1/pair/*` en el firmware actual.**
Las referencias antiguas se conservan como artefactos históricos en
`docs/PAIRING_BUTTON.md` y ejemplos de pruebas legacy.

## Puesta en marcha

1. Michi Ecosystem prepara durante el flasheo el Wi-Fi, la identidad y la
   membresía firmada por la autoridad Michi Home (sin credenciales en Git).
2. El dispositivo importa su configuración de fábrica y se conecta a Wi-Fi.
3. Se publica por mDNS y anuncia por multicast firmado cuando dispone de
   reloj válido. Si el reloj falla, no saltarse la verificación de firmas.
4. Micro Server descubre el receptor, comprueba pertenencia al mismo Home,
   completa `auth/challenge` y `auth/session`, y abre la sesión RTP.
5. Micro Server envía PCM S16LE a 48 kHz/16 bit/2 canales, PT 97, 10 ms,
   1920 bytes por paquete; Stream valida IP, PT, SSRC y tamaño.
6. Heartbeat cada 10 s renueva un lease de 30 s; si expira, la sesión termina.

**Límites actuales:** el funcionamiento sobre placa real y la salida física
DAC/I2S todavía requieren certificación. El simulador no equivale a
reproducción probada sobre hardware. Recuperación sin botón y seguridad de
producción siguen pendientes de cierre.

## Estructura

```
michi-music-stream/
├── README.md
├── LICENSE
├── docs/             # Documentación técnica
├── firmware/         # Código fuente ESP-IDF (universal)
│   ├── main/         # app_main + Kconfig
│   └── components/   # Componentes michi_* (HTTP, Home identity, session, audio…)
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

Los tests C (`tests/host/`) compilan las fuentes REALES del firmware.
`tests/host/test_michi_home.c` está presente en la copia actual de Drive. Las pruebas host
requieren **libcjson-dev** (Ubuntu/Debian: `sudo apt-get install -y
libcjson-dev`; Arch: `sudo pacman -S cjson`; o un prefix local exportando
`PKG_CONFIG_PATH=<prefix>/lib/pkgconfig`). El runner detecta su ausencia y
falla con el comando de instalación — nunca omite silenciosamente.

### Runner local

`./scripts/run_tests.sh` corre TODA la suite (default, `--full`): sync del
bundle, tests de contrato/schema/ejemplos, simulador (unit/scenarios/HTTP),
E2E completo + reporte referenciado a la versión vendorizada, tests C del host y
los dos scans de hardening (referencias legacy y el gate de hardware de
MS-11). `--quick` corre solo las suites Python rápidas + los scans.
Fail-fast con el nombre exacto de la suite que falla y su código de salida
real; la salida nunca se filtra y el resumen final solo lista las suites
ejecutadas.


## KILLCRITIC H1 — Seguridad y sesiones (pendiente validar localmente)

El aprovisionamiento BLE automático queda desactivado por defecto; solo se admite BLE en builds de laboratorio con PoP de 16+ caracteres no compartido. Errores de NVS y de borrado de staging no deben borrar identidad Home. La modificación de Wi-Fi en un dispositivo ya incorporado todavía requiere transacción USB autorizada de Michi Ecosystem; un factory_cfg normal sobre Home existente se rechaza. La extensión experimental now-playing exige `revision` creciente y Content-Type JSON y mantiene `features.now_playing=false`. Véase `docs/HARDENING_SECURITY_SESSION.md`.

## KILLCRITIC H2 — HTTP estricto y diagnóstico de conectividad (pendiente de pruebas)

- Se rechazan cuerpos JSON con basura después del objeto, NUL embebido o escape Unicode NUL. Los campos `now-playing` requieren UTF-8 correcto sin controles.
- Un `Content-Type` incorrecto devuelve 415 real (`Unsupported Media Type`), no un código HTTP 500 con error contradictorio.
- El diagnóstico autorizado añade `wifi.link_associated`, `wifi.ip_ready`, `wifi.provisioned` y `wifi.ipv4`. `wifi.connected` ahora significa IP utilizable: tener RSSI no es prueba de conectividad IP.
- Las métricas de red son observaciones locales, no confirman aceptación por Micro Server ni éxito de mDNS/Whisker.
- No se modificaron RTP, OTA, los secretos ni los contratos congelados Michi Link alpha.5. Pruebas ESP-IDF, host y dispositivo físico pendientes de OpenCode. Ver `docs/H2_KILLCRITIC_DELIVERY.md`.

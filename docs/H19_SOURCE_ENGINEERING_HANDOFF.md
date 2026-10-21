# Michi Music Stream H19 — cierre parcial de observabilidad y seguridad

Fecha local: 2026-10-19. Base: **H16R-PRETEST** en firmware, **H18** en harness de RTP real. No hay ejecución local de compiladores ni tests (flujo del usuario).

## 1. Veracidad de la cadena RTP → ring I2S

Las capas conservan la semántica anterior de `session.state=playing` para no romper v1-lite, pero esa etiqueta solo confirma el estado del contrato, no audio analógico. H19 añade **evidencias de software** por sesión RTP:

- `pcm_packets_submitted` — paquetes RTP válidos admitidos exitosamente en el ring I2S; **no** cuenta silencio generado por pérdidas y underrun.
- `pcm_bytes_submitted` — suma de longitudes PCM admitidas.
- `last_pcm_submit_monotonic_us` — timestamp monotónico del último ingreso PCM.
- Reset de estas métricas junto con el reset preexistente de s_metrics al aceptar un START de sesión.

En `GET /api/v1/receiver-lite/diagnostics` (autorizado) se añade `audio_observation` sin retirar campos previos:

| Campo | Significado/limitación |
|---|---|
| `stage` | `inactive`, `waiting_for_rtp`, `buffering`, `pcm_submitted`, `paused` o `i2s_faulted` |
| `rtp_worker_active` | Indicador del motor, no aceptación remota por Micro Server |
| `i2s_faulted` | Error I2S observable |
| `rtp_packets_accepted` | Contador RTP por sesión |
| `pcm_packets_submitted` | Entregas RTP al ring completadas |
| `pcm_bytes_submitted` | Bytes admitidos al ring |
| `last_pcm_submit_monotonic_ms` | Tiempo de ingreso medido por el reloj monotónico del receptor, cero si ninguno |
| `pcm_ring_admission_observed` | Hubo admisión de PCM en la sesión, **no** prueba de continuidad |
| `analog_audio_verified` | Siempre `false`: esta API no mide analógico |

Snapshot entre módulos no atómico: DELETE/START concurrente puede cambiar la sesión entre lecturas; NO vender como captura consistente de una sola transacción o prueba de audibilidad. Si Micro Server consume el nuevo bloque debe tratarlo como diagnóstico provisional y conservar compatibilidad con receptores sin él.

## 2. Seguridad Wi-Fi

- Los buffers locales `password` y `wifi_cfg` se borran mediante escrituras `volatile` resistentes a eliminación como stores muertos, tanto al configurar Wi-Fi como en fallos de `esp_wifi_start` o `esp_wifi_set_config`.
- Ya no se registra el SSID durante la lectura de credenciales; no se altera NVS Home ni el driver Wi-Fi.
- Nuevo Kconfig `MICHI_WIFI_REQUIRE_WPA2`: si está activado, exige autenticación WPA2-PSK o más fuerte; por defecto `n` para respetar dispositivos existentes. Para un release en LAN protegida, evaluar activarlo. No garantiza cifrado de RTP, TLS ni Secure Boot.
- El código aún usa `WIFI_STORAGE_FLASH` (driver Wi-Fi) además del namespace aplicativo NVS; requiere decisión arquitectónica antes de prometer "única copia persistente". No se cambia automáticamente, ya que puede afectar el flujo de aprovisionamiento.
- Los comentarios de recuperación dejan de sugerir botón físico: la producción recupera por USB autorizado desde Ecosystem.

## 3. Pruebas nuevas para ejecutar después

- `tests/host/test_michi_audio_diagnostics.c`: tabla de estados software.
- `tests/host/test_michi_wifi_secret.c`: borrado explícito de buffer y longitud cero.
- `tests/host/test_michi_audio_engine.c`: comprobación de la contabilización RTP real vs silencio y reset al nuevo START.
- `tests/host/Makefile`: los dos objetivos nuevos forman parte de `make test`, `all` y `clean`.

**No ejecutado**: compilación host, pytest, ESP-IDF, CI, pruebas ESP32-S3, DAC ni integración Micro Server. Todo FAIL de compilación que se detecte en la etapa local debe corregirse antes de certificar.

## 4. Preservación contractual

NO modificados: RTP PT97, S16LE 48kHz 16/2, payload 1920B, SSRC/IP, entidad Home, NVS membership, Whisker/mDNS, OTA A/B, QR, recuperación sin botón, identidad Link. `audio_observation` es añadido no obligatorio a diagnósticos; no cambia el schema de sesión v1-lite.

## 5. Trabajo que sigue dentro de Drive

1. Auditoría estática final con paridad simulator y contrato de diagnostics, seguridad OTA/NVS/Home en rutas no auditadas, sin sustituir componentes aprobados por mocks.
2. Árbol de distribución **íntegro generado desde el contenido real de Drive**, sin `.git`, artifacts build ni secretos y con hash de cada entrada; el overlay H19 no cumple ese objetivo por sí solo.
3. Revisión final de fronteras y archivos históricos para evitar descargar versiones solapadas.

El 100% de Drive no incluye la ejecución de tests/hardware por petición del usuario; **este reporte NO declara que el 100% está alcanzado**.

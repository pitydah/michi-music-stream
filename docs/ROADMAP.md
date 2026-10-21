# Michi Music Stream — Roadmap (reconciliado con el código)

## Implementación disponible

- Firmware ESP32-S3 modular con Home membership y `michi_home` / `michi_auth`.
- Aprovisionamiento de fábrica de Wi-Fi e identidad (MICHI-F1) por medio del flujo Ecosystem.
- Descubrimiento mDNS, multicast firmado condicionado por hora válida.
- Transporte RTP/UDP, sesión única, heartbeat, PCM S16LE 48 kHz/16 bit/2 canales.
- UI de 320×240, diagnósticos, salida I2S, detección DAC, simulador y pruebas host.

## Trabajo necesario para cierre

1. **Fuente de tests host recuperado:** `tests/host/test_michi_home.c` está presente. Exigir compilación limpia en lugar de utilizar el ejecutable incluido en la copia.
2. **Convergencia de certificación:** etiquetar informes E2E con la versión real del bundle. En esta copia es alpha.5; los resultados preexistentes `michi-link-alpha6.json` son históricos y NO certifican alpha.6.
3. **Interoperabilidad real:** demostrar descubrimiento, confianza Home, autenticación y audio Micro Server→ESP32-S3 en red local y mostrar estados en WebUI.
4. **Recuperación sin botón:** definir y validar reflasheo seguro, pérdida de Wi-Fi, identidad corrupta y rollback OTA.
5. **Validación Hi-Fi:** salida PCM5122/PCM5102A, reconexiones, jitter, latencia, underruns y escucha prolongada en placa física.
6. **Seguridad de producción:** revisar TLS, Secure Boot, Flash Encryption, canales de OTA y protección de secretos.

## Futuro, sin publicidad de soporte actual

PCM 24-bit/96 kHz, otros formatos, RTCP, corrección de drift y multiroom sincronizado requieren implementación y evidencia física antes de anunciarse.

## Fuera de alcance

Biblioteca musical local, playlists, indexación, vídeo y reproducción autónoma.

## Sprint A/B de integración (edición en Drive)

- `now-playing`: handler local con comprobación de Bearer, token, identidad,
  sesión y lease; el feature flag permanece false hasta la prueba E2E y
  **Micro Server debe implementar el productor compatible**.
- CI de assets: pipeline de 2 bits coherente con `sdkconfig.defaults`, más
  reproducción determinista de iconos de precisión UI-06.
- UI: fuente/ubicación en esquina superior derecha, sin invadir el footer.
- La incorporación Wi-Fi vía flasheo de Ecosystem, seguridad de fabricación,
  mDNS/Whisker y reproducción real siguen pendientes de DEVICE_E2E.
- La paquetización RTP de 10 ms/1920 bytes no cambia en este sprint: cualquier
  reducción exige un cambio de contrato coordinado con Micro Server y Link.

## KILLCRITIC H1 — Seguridad y sesiones (pendiente validar localmente)

El aprovisionamiento BLE automático queda desactivado por defecto; solo se admite BLE en builds de laboratorio con PoP de 16+ caracteres no compartido. Errores de NVS y de borrado de staging no deben borrar identidad Home. La modificación de Wi-Fi en un dispositivo ya incorporado todavía requiere transacción USB autorizada de Michi Ecosystem; un factory_cfg normal sobre Home existente se rechaza. La extensión experimental now-playing exige `revision` creciente y Content-Type JSON y mantiene `features.now_playing=false`. Véase `docs/HARDENING_SECURITY_SESSION.md`.


## Sprint H2 (pendiente certificar)

- **HTTP:** lectura estricta de cuerpos completos y errores semánticos (415); contratos experimentales ahora rechazan cadenas malformadas en UTF-8.
- **Diagnóstico Wi-Fi:** distingue STA asociado a AP de conectividad IPv4; nuevas señales para la WebUI de Micro Server sin declarar reconocimiento remoto.
- **Regresiones host:** casos contra NUL (raw y escape), basura tras JSON y UTF-8 inválido en el parser real.
- **Todavía bloqueantes:** recuperación USB autenticada; productor de now-playing en Micro Server con revisión creciente; descubrimiento Whisker/mDNS real; MTU y DAC físico; OTA/A-B y canal de control protegido.

## Sprint H3 — Integración reproducible sin declarar éxito físico

- Simulador Flask: ruta experimental `PUT /receiver-lite/now-playing` con
  autenticación Bearer, propietario de sesión, token RTP y revisión creciente.
- Diagnóstico seguro del firmware: `session.metadata_revision` para correlación
  entre lo enviado por Micro Server y lo realmente aceptado por Stream.
- Regresiones de orden, autorización cruzada, caducidad y límites de JSON;
  sin ejecutar tests ni compilación ESP-IDF en Drive.
- Sigue pendiente implementar el productor en Micro Server y mostrar las
  métricas correlacionadas en su WebUI (no anunciar soporte hasta E2E).
- Persisten los bloqueantes de recuperación USB, RTP/MTU, OTA y
  certificación de audio por PCM5122 en placa física.


## Línea de cierre de fuente en Drive: H8–H15 (2026-10-19)

- H8–H12: ramp PCM, protección DMA/I2S, ring, QUIESCE y recursos concurrentes.
- H13–H14: atomics de RTP, lease I2S, transferencia de propiedad del worker,
  arranque transaccional con socket/memoria antes de reiniciar I2S y deadline PCM real.
- **H15 (base de fuente vigente):** política de PAUSE revalidada después de
  recvfrom(), clasificación correcta de escrituras interrumpidas por STOP/QUIESCE,
  reconciliación de sesiones RTP terminadas aunque I2S falle, referencias de
  limpieza de START fallido, generación del watchdog atómica, `esp_timer_restart`
  fail-closed, enum HTTP 500 para fallo de renovación, métricas ligadas a una
  sola sesión, cobertura host nueva y documentación reconciliada.
- **H16 — por cerrar dentro de Drive:** harness que compile y ejecute el
  `michi_audio.c` real con dobles de sockets, PSRAM e I2S; pruebas deterministas
  de PAUSE en recvfrom, fault I2S, STOP y START concurrentes, y wiring al CI.
- **H17 — por cerrar dentro de Drive:** matriz OTA/Home/Wi-Fi y guardrails de
  seguridad/recuperación, pruebas de fallos encadenados, claridad de diagnósticos.
- **H18 — por cerrar dentro de Drive:** snapshot integral limpio de TODO el
  repositorio, clasificación de generados e históricos, auditoría de contratos
  congelados y manifiesto único con alcance realmente completo.

### Definición de 100% de trabajo de Drive

Consiste en haber revisado y preparado las fuentes, contratos, tests y documentación
que pueden modificarse en este entorno, con ningún P0 estático abierto conocido,
SIN requerir ejecutar compilaciones/tests ni dispositivos físicos. Este objetivo
NO constituye certificación de funcionamiento. Las pruebas en OpenCode o Antigravity
son una etapa separada después de descargar y reemplazar la copia local.

**Al 19 de octubre, H15 aún no satisface completamente esa lista de cierre.**
No alterar unilateralmente PT=97/1920B ni las firmas Michi Link.


## H16R–H19 — Estado vivo de fuente (2026-10-19, Drive)

- **H16R, motor de producción:** RTP e I2S H15, watchdog autónomo de
  1 segundo y recuperación de código del jitter buffer; preservados.
- **H18, tests preparatorios:** `test_michi_audio_engine.c` compila el RTP
  real con UDP loopback y shims; el generador de snapshot exige módulos
  esenciales y falla ante exportaciones parciales de Drive.
- **H19, observabilidad y hardening:** métricas por sesión de PCM RTP
  admitido por el ring I2S; `audio_observation` en diagnósticos autorizado;
  test de clasificación; borrado volátil de credenciales Wi-Fi temporales;
  Kconfig optativo de WPA2 mínimo; pruebas host preparadas.
- **Semántica:** `session.state=playing` sigue siendo el contrato legacy
  v1-lite y NO evidencia audio audible. `audio_observation.stage` informa
  hitos software, no el DAC analógico; `pcm_packets_submitted` tampoco
  demuestra reproducción DMA completa.
- **Pendientes de cierre Drive:** inspección de los contratos completos de
  Home/OTA/NVS, snapshot integral generado desde la totalidad de Drive,
  revisión de divergencia simulador/firmware y eliminación segura de
  historial/staging redundante. No declarar 100% por producir un overlay.
- **Excluido del porcentaje Drive:** ejecutar pytest/make/CI, ESP-IDF,
  red física, PCM5122 y pruebas con Micro Server/WebUI. Estos pasos tienen
  sus propias puertas de aceptación al sustituir la copia local.

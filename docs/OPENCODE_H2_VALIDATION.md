# OpenCode — Pruebas locales H2 (Michi Music Stream)

Trabaja sobre la copia local después de descargar Google Drive. Preserva `.git`, NVS, claves, firmware activo y secretos. Reporta errores antes de tocar código no relacionado. No consideres un build como prueba física.

1. Inspecciona `git status --short`, `git diff --check`, `git diff --stat`. Verifica bundle Michi Link alpha.5, `features.now_playing=false` y versiones de toolchain.
2. Compila desde limpio `tests/host` y ejecuta `bash scripts/run_tests.sh --full`. No uses ejecutables de host antiguos. Conserva stdout y exit codes sin filtrarlos.
3. Compila el firmware con ESP-IDF 5.3/ESP32-S3. Comprueba las APIs y compatibilidad de `cJSON_ParseWithOpts`, `cJSON` y `MALLOC_CAP_SPIRAM`.
4. Verifica el parser real `michi_http_json_parse_strict` para JSON válido con espacios, cuerpo vacío, basura tras objeto, JSON array, NUL en bytes, escape `\\u0000`, backslash escapado literal y sobrelongitud. Verifica UTF-8 válido (ñ, tildes, emojis) e inválido (overlong, surrogate, truncamiento, caracteres de control).
5. Prueba HTTP `PUT /api/v1/receiver-lite/now-playing` con Home autorizado y lease activo: `Content-Type: application/json` esperado; ausente/incorrecto debe responder **HTTP 415**, código de error `INVALID_REQUEST`; un token o sesión ajenos deben ser rechazados. No dejar actualización visual si falla la validación.
6. Comprueba regresiones de `now-playing`: revisión 1/2, duplicado o atrasado 409; revisión negativa/fraccionaria/fuera de rango 400; `Content-Type` con parámetros y sin parámetros, payloads con JSON extra, NUL escapado, bytes inválidos y título en español. Si se desea aceptar parámetros de charset, actualizar contrato y test conjuntamente: la política actual es coincidencia exacta.
7. Consulta diagnósticos autorizados cuando el Wi-Fi está (a) sin credenciales, (b) asociado sin IP, (c) con IP, y (d) desconectado. Verifica `link_associated`, `connected`, `ip_ready`, `ipv4`, `rssi_dbm` y la ausencia de secretos.
8. Repite pruebas de concurrencia de sesiones del sprint H1; demuestra que cierre/reconexión y `revision` no arrastran canciones, ni se bloquean mutex `session` y `display`.
9. Después, dispositivo físico: validar LCD, descubrimiento desde Micro Server WebUI, audio PCM5122/I2S, RTP/MTU, estrés de memoria y OTA. No marcar DEVICE_E2E_PASS sin prueba real.
10. Entrega informe PASS/FAIL, cantidad exacta de tests, errores, logs relevantes y commit recomendado. No flashear equipo con identidad real ni borrar NVS sin aprobación del usuario.

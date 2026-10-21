# Michi Music Stream — H2 KILLCRITIC: parsing estricto y diagnóstico verificable

## Alcance guardado como fuente (no certificado)

Archivos modificados:
- `firmware/components/michi_http/http_server.c`
- `firmware/components/michi_http/json_helpers.c`
- `firmware/components/michi_http/include/michi_http.h`
- `tests/host/test_json_helpers.c`
- `README.md` y `docs/ROADMAP.md`

### 1. HTTP y JSON

El handler de now-playing ya pedía `Content-Type: application/json`, pero el mapeador omitía HTTP 415. Se agrega `415 Unsupported Media Type` y un sobre de error `INVALID_REQUEST` para ese caso experimental.

Todos los handlers que usan `read_json_body()` pasan a un parser que requiere un objeto **completo** y rechaza bytes posteriores, NUL en el cuerpo, y escape Unicode NUL (`\\u0000`) dentro de cadenas. El propósito es evitar que cJSON y las rutinas de C discrepen sobre el límite de la carga útil. Los textos de `now-playing` se validan como UTF-8 escalar correcto, además de los límites en bytes y ausencia de controles.

La extensión `now-playing` sigue siendo experimental: `features.now_playing=false`, y se mantiene su control de autorización de Home, token de sesión y revisión monótona. Los requisitos del bundle alpha.5 no se modifican.

### 2. Semántica Wi-Fi para WebUI

`GET /api/v1/receiver-lite/diagnostics` (requiere autorización existente) añade:
- `wifi.link_associated`: respuesta válida del controlador STA para RSSI.
- `wifi.ip_ready`: se obtuvo IP vía `michi_wifi_get_ipv4()`.
- `wifi.connected`: alias compatible de `ip_ready` (antes reflejaba únicamente RSSI).
- `wifi.ipv4`: dirección actual; cadena vacía si no se ha obtenido.
- `wifi.provisioned`: existen credenciales Wi-Fi locales.

`wifi.rssi_dbm` solo aparece si el enlace está asociado. No interpretar estas observaciones como prueba de que Micro Server aceptó Whisker, mDNS o autorizó sesión. Esa confirmación deberá instrumentarse en la WebUI del servidor.

### 3. Exclusiones deliberadas

- Sin nuevo protocolo de recuperación sobre USB ni sustitución de identidades Home.
- Sin cambios de la paquetización RTP, buffers I2S, codec anunciado, OTA o autentificación Home.
- No se modificó el simulador ni se prometió soporte compatible con Micro Server. La extensión de metadatos se mantiene experimental.

### 4. Criterio de entrega

Esta edición modifica exclusivamente fuentes y casos de prueba. No se ejecutaron tests, no se compiló ESP-IDF y no hay DEVICE_E2E_PASS. OpenCode debe comprobar la compilación C/ESP-IDF, los tests host, contratos existentes, diagnóstico WebUI y los escenarios negativos indicados en el prompt H2.

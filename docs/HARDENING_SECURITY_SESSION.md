# Michi Music Stream — KILLCRITIC H1: seguridad, sesiones y metadatos

**Estado:** archivos fuente listos para validación local. No se ha ejecutado la suite de pruebas ni compilado el firmware ESP-IDF en este trabajo.

## Implementaciones

- `michi_wifi/Kconfig` / `michi_wifi.c`: BLE de aprovisionamiento desactivado por defecto para producción. Solo un build explícito con `CONFIG_MICHI_WIFI_ALLOW_BLE_PROVISIONING_LAB=y` puede iniciar la ruta BLE y exige PoP propio de 16+ caracteres; `michi123` o PoP vacío se rechazan. El borrado de Wi-Fi se rehúsa en builds de producción para no dejar el equipo sin mecanismo de configuración.
- `app_main.c`: **no se borra NVS automáticamente** ante errores de inicialización. Contiene identidad criptográfica, Home y Wi-Fi; la recuperación exige una actuación autorizada desde USB.
- `factory_cfg.c`: reconoce una partición de fábrica no vacía sobre Home ya provisionado y **rechaza** sobrescribirlo; no borra una identidad importada cuando falla el borrado de la partición de staging. Esto no actualiza Wi-Fi en un Home ya existente.
- `michi_session.c/.h`: la limpieza de metadatos se realiza bajo el mutex de sesión en stop, abort, reconciliación de engine y watchdog; el token nuevo se copia protegido y se ordenan los eventos con la misma sesión; las actualizaciones usan revisión estrictamente creciente, reiniciada en cada sesión.
- `http_server.c`: `now-playing` exige Content-Type `application/json`, un nuevo campo JSON entero `revision` (1..4294967295), permiso Home y token RTP; revisión repetida/atrasada da 409. Sin datos públicos que anuncien esta capacidad hasta integración con Micro Server.
- `tests/host`: fake visual actualizado y casos para revisiones, propietarios y sesiones incorrectas, reinicios de sesión y rechazo de staging.

## Pendientes críticos

1. **Recuperación real USB de un Home existente:** requiere contrato firmado por autoridad Home, autorización, nonces/antirreplay, operación transaccional de NVS y herramienta en Ecosystem. Un blob `factory_cfg` normal no es autorización de recuperación y por eso se rechaza. No publicitar la actualización de Wi-Fi como completada.
2. **Contrato Michi Link:** `revision` es una ampliación local de laboratorio no negociada con Micro Server; `features.now_playing=false` debe seguir hasta que se implemente el productor y se homologuen tests de los dos extremos. Cambio incompatible con el borrador previo de cuatro campos.
3. **Red y audio:** no se han cambiado RTP/MTU, I2S/DAC o el motor OTA. Esos cambios necesitan integración multi-repo, mediciones reales y ensayos de recuperación.
4. **BLE build:** en producción el servicio no arranca, pero pueden seguir vinculándose dependencias Bluetooth; revisar exclusión completa del código y soporte de recuperación antes de retirarlas.
5. **Errores tempranos de pantalla:** el BSP no se inicializa antes de todos los fallos NVS. Todavía no puede garantizarse mensaje visible en LCD para ese tipo de error.

## Precauciones al descargar

Preservar `.git` local, particiones y claves; revisar diferencias en OpenCode ANTES de flashear. La secuencia de locks `session -> display` debe auditarse con concurrencia real. El simulador y la WebUI de Micro Server aún no implementan la revisión de metadatos.

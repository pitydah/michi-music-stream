# Prompt para OpenCode — validación Michi Music Stream H1 (solo LOCAL)

Trabaja en mi repositorio local de Michi Music Stream después de reemplazar los fuentes modificados en Google Drive, respetando `.git` y sin sobrescribir identidades ni credenciales. Valida el nuevo sprint de hardening y corrige los fallos que encuentres, sin crear funcionalidades ajenas al proyecto.

1. Muestra `git status --short`, `git diff --stat`, `git diff --check`; identifica versión Michi Link y los archivos exactos del sprint.
2. Revisa Kconfig y todas las ramas de arranque de `michi_wifi.c`; el build de producción jamás debe arrancar BLE automáticamente con PoP común, ni borrar Wi-Fi sin recuperación autorizada. Prueba build laboratorio con PoP vacío/default y con PoP único.
3. Reconstruye host desde cero y ejecuta `bash scripts/run_tests.sh --full`. No uses binarios host previos ni silencies errores; valida mocks de `michi_display_update_now_playing` y los nuevos tests de metadatos/identidad.
4. Construye ESP-IDF 5.3 para ESP32-S3. Revisa warnings, tareas, stacks, locks, colas, símbolos de sdkconfig, C11 y uso de memoria. Ajusta únicamente lo demostrado necesario.
5. Ejecuta pruebas HTTP positivas y negativas: `revision` 1/2, duplicados o fuera de orden 409, cero/negativo/fracción/overflow/NaN 400, Content-Type ausente/incorrecto 415, JSON campos duplicados o desconocidos, longitudes, UTF-8, control embebido, sesión ajena/caducada, token y Bearer, carreras de teardown y nuevas sesiones.
6. Ejecuta stress entre stop/abort/watchdog, nueva sesión y entrega de metadatos; verifica ausencia de limpieza tardía y de deadlocks entre mutex de sesión/display. Comprueba que no se amplía el lease RTP al actualizar metadatos.
7. Fuerza errores NVS y de borrado de factory_cfg; debe conservarse identidad ya almacenada y rechazar staged overrides en un Home existente. No dar por terminado el cambio de Wi-Fi por USB: falta contrato autenticado y trabajo en Ecosystem.
8. Confirma `features.now_playing=false`, que el simulador y Micro Server no sean anunciados como compatibles con el nuevo contrato experimental de cinco campos y que Michi Link alpha5 no se haya alterado.
9. Para pruebas físicas posteriores: medir arranque y diagnóstico LCD, mDNS/Whisker, autorización Home, recepción RTP, jitter, I2S/PCM5122, animaciones y OTA. No declarar `DEVICE_E2E_PASS` sin evidencia física.
10. Entrega informe de PASS/FAIL con commit, resultados, números de tests, errores y siguientes correcciones. No borres NVS ni ejecutes factory reset en hardware sin mi aprobación explícita.

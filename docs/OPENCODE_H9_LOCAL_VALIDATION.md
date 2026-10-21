# Prompt para OpenCode — Michi Music Stream H9 + H8 consolidado

Trabaja solo en el repositorio **local** Michi Music Stream, mantén `.git` y registra HEAD de partida. El ZIP H9 contiene una entrega completa sobre H8: copia su estructura a la raíz (NO mezclar archivos H8 parciales de Drive con otros H7). No modificar Michi Link, Ecosystem ni Micro Server. Nunca subir ni publicar credenciales, claves de Home, datos personales o `sdkconfig` privado.

## Auditoría y ejecución obligatoria

1. Presenta `git status --short`, SHA, diff completo y árbol efectivo H7→H8→H9. Detecta cualquier edición simultánea local: no sobrescribir ni deshacer cambios de terceros, concilia los diffs.
2. Ejecuta `make -C tests/host test` y pruebas relevantes individuales (`test_michi_volume`, `test_michi_audio_output`, `test_michi_session`, RTP/jitter, auth HTTP). No ocultes fallos ni omitas su causa. Revisa tipos, include paths, C11 `_Atomic`, semántica de `pdMS_TO_TICKS()` en 100 y 1000 Hz y enlazado del stub `michi_volume_fake.c`.
3. Compila firmware con `idf.py build` para ESP32-S3, sin actualizar el contrato ni la versión de bundle. Examina errores link de `michi_audio_output_get_quality` en HTTP, CMake `REQUIRES`, ABI de cabeceras y diferencias host/ESP-IDF.
4. Fault injection de DMA: 1 fallo aislado recuperable; 3 fallos consecutivos fuerzan FAULTED; `written < n` con `ESP_OK` cuenta error; STOP apaga el canal real después del worker fatal; writer en FAULTED rechaza. El test host de fallo persistente está preparado, pero debe verificarse que el shim permita inyección estable. Valida recuperación STOP→START sin carreras.
5. Backpressure: programa una situación donde I2S deja de drenar con productor activo; espera `ESP_ERR_TIMEOUT` después de ~1.5s de ticks bloqueados sin spin o watchdog; comprueba que la sesión y su token terminan correctamente. Verifica que el tiempo no se reinicia en cada progreso intermitente.
6. Rampa H8: testear 16/24 bit, mono/estéreo, 8/44.1/48/96k, cambios de mute y volumen intercalados, retarget a mitad del fade, muestra positiva y negativa, sin desbordes; no atribuir volumen físico no medido.
7. `GET /api/v1/receiver-lite/diagnostics`: conserva Bearer Home, anterior `i2s_errors`, y reporta `i2s_quality` sin secretos. Incluir tests 401/403, campo nuevo, estado FAULTED, OOM JSON si el harness lo admite.
8. CI GitHub/packaging: reportar SHA, jobs reales, diferencias entre compilación ESP-IDF y host, fails y skipped. No etiquetar simulación como test físico.
9. Hardware (por realizar con usuario): placa Waveshare ESP32-S3-LCD-2, PCM5122, I2S, capturas de conexión Micro Server, latencia y underruns; 24 h bajo carga, reconexión Wi-Fi, OTA/rollback en placa de prueba separada. Contrasta con métricas de WebUI cuando estén integradas.

## Entrega de OpenCode

- Informe KILLCRITIC PASS/FAIL/SKIP por categoría con comando, log, entorno, commit SHA y causa de cada FAIL.
- Repara tests o código defectuoso que detectes, sin desactivar assertions ni cambiar contratos protegidos.
- Antes del merge confirma semántica de prefill, health, STOP/QUIESCE, handshakes Home, seguridad BLE, y retrocompatibilidad de `diagnostics`.
- Da un **porcentaje estimado respaldado por evidencia**. El 78 % es objetivo condicionado, no autoevaluación por commits.

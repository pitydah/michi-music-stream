# Prompt de validacion LOCAL — OpenCode / Codex (NO ejecutar en Google Drive)

Trabaja exclusivamente sobre la carpeta local descargada de Google Drive,
con la version Sprint A/B de Michi Music Stream. Antes de modificar nada,
identifica `git status --short`, `git diff --stat`, revision real, versiones
ESP-IDF, `contracts/michi-link/VERSION` y `UPSTREAM_COMMIT`. **No descargues
ni sustituyas contratos** sin compararlos con los repositorios Michi Link y
Micro Server. No consideres los binarios precompilados una prueba.

1. Reconstruye desde cero host y CI: `make -C tests/host clean`,
   `./scripts/run_tests.sh --full` y job `ui-preview`. Presta especial
   atencion a assets 2 bpp y `gen_precision_icons.py` y a la presencia
   de `tests/host/test_michi_home.c`. Reporta el primer fallo, no lo ocultes.
2. Compila ESP-IDF 5.3 para ESP32-S3 (sin flashear hasta revision de diff).
   Revisa headers, CMake includes, enums de resultado y stack HTTP: la nueva
   funcion en `michi_session` es invocada desde HTTP y debe ser compatible.
3. Agrega pruebas positivas y negativas REALMENTE EJECUTADAS para PUT
   `/api/v1/receiver-lite/now-playing`:
   - bearer invalido/sin permiso, token X-Michi-Session invalido;
   - session_id obsoleto o ajeno y controlador distinto;
   - sesion inexistente/cerrada/lease expirado y carrera con DELETE;
   - JSON nulo, JSON no objeto, 4 claves obligatorias, claves duplicadas,
     mas campos, tipos no-string, longitudes en frontera, UTF-8, controles;
   - 200 solo con datos de la sesion, no prolonga lease, reemplazo de
     metadatos, cleanup en cierre/abort/reinicio;
   - `features.now_playing=false` mientras no exista E2E con productor.
4. Ejecuta pruebas de UI a 320×240: playing, paused, buffering, sin
   metadata, titulo UTF-8 largo, fuente larga y label en esquina superior
   derecha sin superponerse con estado ni invadir footer. Render UI06 wave.
5. Prepara PR independiente en Michi Micro Server para productor con
   `session_id`, bearer y token correctos. Avanza conjuntamente la WebUI:
   debe exponer etapas y diagnosticos red, auth, sesion, RTP, DAC,
   errores reales y distinguir TXT enviado de receptor visto.
6. Solo con placa ESP32-S3/PCM5122 real: flasheo por Ecosystem,
   Wi-Fi/clock/mDNS/Whisker, autenticacion Home, metadata real, RTP audio,
   reconexion, jitter, underruns, OLED/LCD DMA vs audio y OTA segura.
   Documenta hardware, red, SNTP y resultados cuantitativos.
7. NO elimines BLE ni cambies RTP 10ms unilateralmente. Planifica migracion
   de fabrica/recovery y propuesta de paquetizacion con Micro Server/Link.

Entregables: tabla de PASS/FAIL/SKIP con razones, logs concisos,
configuracion de hardware, revision Git, fallos y fixes, estado real de
metadatos y planes de integracion pendientes. Nunca declares 100% por
compilar o por usar el simulador.

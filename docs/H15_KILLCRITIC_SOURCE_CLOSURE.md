# Michi Music Stream H15 — Auditoría KILLCRITIC de ingeniería de fuente

**Fecha:** 2026-10-19 · **Base:** H14 validado por hash · **Destino:** carpeta original de Drive.
**Tipo:** source-only, sin compilar ni ejecutar tests de acuerdo con el flujo del usuario.

## Resumen ejecutivo

Esta pasada corrige defectos de concurrencia y consistencia encontrados tras H14.
Se preserva estrictamente el contrato RTP (48kHz, 16-bit, 2 canales, PT=97,
10ms, 1920 bytes), Home Membership, Whisker/mDNS, OTA A/B, Wi-Fi precargado
sin botón de pairing, y los estados de las APIs existentes salvo añadir un
resultado de error seguro del heartbeat (HTTP 500 al fallar watchdog).

**H15 no se considera certificación funcional, ni cierre al 100% de todo el
trabajo realizable desde Drive**: faltan especialmente las pruebas del motor
RTP real, algunas regresiones de fallos encadenados y la distribución integral.

## A. RTP/PCM: PAUSE en un recvfrom() bloqueante — P0 resuelto en fuente

Antes de H15, el worker capturaba `s_paused` ANTES de `session_recv()` y usaba
ese valor DESPUÉS de un socket timeout. Mientras tanto, el control podía
confirmar QUIESCE. Una posterior escritura retornaba INVALID_STATE y el
motor podía autodestruirse, aunque PAUSE fuese válida.

Cambios: `michi_audio_session_set_paused_impl()` prepublica pausa antes de
esperar QUIESCE; `session_task()` calcula la acción después del recv; las
escrituras PCM interrumpidas por STOP no generan falso error; si I2S está
QUIESCED y PAUSE está activa, se limpia jitter y se continúa sin destruir RTP;
si I2S está FAULTED, el fallo sigue siendo terminal. RESUME solo abre la
admisión RTP después de que I2S confirme su transición.

Helper puro `michi_audio_pause_policy.h` utilizado por el motor y por el
nuevo `test_michi_audio_pause_policy.c`. Su test es determinista de contrato;
**no sustituye** un test del módulo RTP real bajo eventos bloqueantes.

## B. Teardown RTP e I2S — P0 resuelto en fuente

`session_teardown_locked()` distingue:
- Fallo de stop con worker todavía activo: conservar identidad/credencial,
  restaurar estado y requerir reintento.
- Fallo del silencio I2S cuando RTP ya terminó: cerrar sesión, borrar
  token y metadata, cancelar lease y registrar `SESSION_CLOSED`, pero
  **propagar** el error de hardware al solicitante; no publicar reproducción.

`michi_session_active()` y el heartbeat ahora reconcilian trabajadores RTP
terminados, sin esperar al GET de información.

## C. START transaccional y rollback incompleto — P0 resuelto en fuente

Un fallo posterior a levantar RTP (consulta de puerto o watchdog) podía
encadenarse con un stop fallido. H15 retiene `s_startup_cleanup_pending` y
obliga a reconciliar el motor pendiente antes de permitir otro START. Esta
lógica no declara una sesión fantasma activa: devuelve error y preserva el
ownership hasta su recuperación. Fake host con inyección de lectura de puerto
fallida y prueba de reintento.

## D. Watchdog de lease — P0/P1 resuelto en fuente

- `s_lease_generation` usa `_Atomic uint32_t`: el callback no lee una variable
  ordinaria modificada por otras tareas.
- Heartbeat usa `esp_timer_restart()` y NO `esp_timer_start_once()` sobre un
  temporizador potencialmente activo.
- No consume número de secuencia ni extiende deadline ante error de rearme;
  devuelve `MICHI_SESSION_HEARTBEAT_WATCHDOG_ERROR` -> HTTP 500.
- Si la cola del watchdog se satura, el callback no bloquea ni borra sesión:
  intenta un nuevo disparo breve y registra el resultado del timer.
- El hilo receptor registra cualquier error de rearmado de watchdog,
  incluyendo el fallo al retrasar expiración o reintentar un cierre incompleto.

**Residual:** si la cola se satura y también falla el reintento del timer,
la expiración autónoma puede quedar degradada; el deadline se sigue
comprobando en rutas de API. Añadir fault injection determinista H16.

## E. API y snapshot de métricas — P1 corregido

`michi_session_get_info()` ahora mantiene el mutex de sesión mientras
reconcilia el bus de estados y copia contadores RTP. De lo contrario,
DELETE seguido de nuevo START podía devolver ID anterior con métricas nuevas.
Los logs de rechazo heartbeat capturan IP o secuencia antes de soltar mutex,
evita leer buffers borrados o reasignados a otra sesión.

## F. Tests fuente agregados

- `test_h15_watchdog_restart_fail_closed`: no se prolonga lease ni consume
  secuencia si falla restart; un nuevo intento puede recuperarse.
- `test_h15_failed_output_quiesce_closes_dead_transport`: error I2S se
  propaga pero la sesión ya destruida no permanece activa.
- `test_h15_heartbeat_reconciles_dead_engine`: nunca renueva lease zombie.
- `test_h15_failed_startup_rollback_retried`: ownership y cleanup antes
  de nueva sesión.
- `test_michi_audio_pause_policy`: tabla exhaustiva de combinaciones
  booleanas de run/pause/output_quiesced.
- `tests/host/Makefile`: nuevo objetivo y ejecución en suite normal.
- `esp_timer` y fake RTP: inyección de errores explícita (solo host).

**Estado:** los casos fueron escritos y conectados pero NO se ejecutaron.

## G. Audio I2S y DAC

Se conservaron controles H8–H14: único propietario DMA, barra de QUIESCE,
volumen y rampa PCM, referencias de ciclo de vida, gestión de fallos y deadline
del productor. No hay evidencia digital o analógica adicional en H15.
La documentación 'sample-clean' no demuestra por sí sola que el DAC físico
haya agotado todos sus buffers. Esa medición queda fuera del trabajo Drive.

## H. Wi-Fi, descubrimiento, Home e identidad

Sin cambios ABI ni contratos durante H15. Las restricciones de seguridad
son explícitas: solo LAN; `WIFI_AUTH_OPEN` permanece como umbral de
compatibilidad en el código, sin afirmar WPA2 como requisito de producto;
Whisker es firmado; el tiempo requiere sincronización; Home Membership
no implica cifrado RTP por paquete.

Queda dentro de la fase Drive una revisión de política configurable Wi-Fi,
regresiones de identidad NVS y errores de reaprovisionamiento. Los flujos
que requieren código de Ecosystem o Micro Server se tramitan en sus repos.

## I. OTA / rollback

`michi_ota_boot_selftest_done()` acepta `PASS` o `DEGRADED`. Una inspección
de `app_main.c` demuestra que el gate trial exige identidad READY, HTTP,
estado, resultado de boot y disponibilidad de audio SI la SKU espera audio.
El camino `DEGRADED` está pensado para variantes sin audio esperado; no
se cambió indiscriminadamente la política y se evitó un rollback no deseado.
La certificación sobre particiones reales continúa fuera del alcance Drive.

## J. Tests del RTP real — deuda prioritaria dentro de Drive

`tests/host/test_michi_session.c` prueba `michi_session.c` real con fake RTP.
Los tests puros del jitter, `rtp_guard`, y `pause_policy` no compilan la
implementación entera de `michi_audio.c`; por tanto no demuestran carreras
reales de sockets/tareas/PSRAM. FALTA un harness que compile directamente
`michi_audio.c` usando socket fake, I2S fake, watchdog y barriera del scheduler.
Tests obligatorios: PAUSE durante recvfrom, STOP durante escritura,
error de bind/alloc/I2S, falla de task create, retry de startup y worker que
se autodestruye. Este vacío NO se puede convertir en PASS documental.

## K. CI / distribución y documentación

El nuevo test puro está conectado a `make -C tests/host test`. El usuario
validará localmente el Makefile. README y ROADMAP reciben sección de
baseline actual y límites; se preservan los documentos históricos como
historia, no se utilizan para declarar PASS H15.
El ZIP cumulativo H8–H15 sigue siendo un overlay, NO un repositorio Git
íntegro, por lo que descargar la carpeta Drive y conservar `.git` local
sigue siendo obligatorio.

## L. Cierre de trabajo Drive versus trabajo local

**Dentro de Drive:** desarrollo de fuente, matriz de fallos, esquemas, suite
host a ejecutar, QA de contratos, documentación y paquete limpio. Nunca
marcar 100% si quedan P0 fuente o tests críticos sin preparar.

**Fuera del porcentaje Drive:** correr compilación, CI, ESP-IDF, conectar
ESP32-S3/PCM5122, ensayos 24h, SNTP, Wi-Fi y RTP reales, interoperar con
Micro Server/WebUI e implementar cambios en otros repositorios. Aun
cuando un test local descubra un defecto nuevo, podrá abrir otro ciclo.

## Criterios pendientes antes de 100% de fuente

1. **H16:** harness de RTP REAL y tests deterministas de fallos/reconexión.
2. **H16:** fault injection del watchdog (queue full + restart fail) y
   política de fail-closed cuando ambos fallan, sin depender exclusivamente
   de llamadas HTTP.
3. **H17:** seguridad y recuperación completa revisada por tabla de rutas:
   fábrica/Home/NVS/Wi-Fi/OTA, sin ampliar unilateralmente protocolos.
4. **H18:** distribución íntegra del repositorio fuente limpia y
   manifiesto único; exclusión de `.git`, builds y caches obsoletos.
5. **H18:** distinguir estado sesión creada/PLAYING de audio audiblemente
   confirmado en diagnósticos, conservando contrato Michi Link.

**Resultado H15:** regresiones conocidas P0 reducidas y entregables
actualizados; **NO** se declara 100% de fuente o hardware.

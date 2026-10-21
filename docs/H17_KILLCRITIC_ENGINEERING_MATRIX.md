# Michi Music Stream — Auditoría H17 del cierre de fuente en Google Drive

**Fecha:** 2026-10-19. **Autoridad de firmware:** H16R-PRETEST. **Incremento de este trabajo:** herramientas de distribución H17, pruebas redactadas e integración en runner. **Estado:** FUENTES PREPARADAS, sin ejecución. **No marcar 100% todavía.**

## Definición formal de 100% Drive

El objetivo es la **completitud estática del trabajo de código y de entrega** que puede prepararse dentro de Google Drive. Se incluyen firmware ESP-IDF, tests redactados, falsificaciones deterministas, simulador, scripts, contracts vendorizados, documentación, CI y una distribución íntegra de fuentes. Se excluyen compilación real, ejecución de tests, integración dinámica Micro Server, hardware, certificación DAC, pruebas prolongadas y cambios a otros repositorios. Un test no ejecutado debe aparecer como `NOT_RUN`, nunca `PASS`.

## Cambio de baseline detectado y reconciliado

Al revisar el árbol se descubrió **H16R**, más reciente que H15/H14, con watchdog autónomo y jitter-buffer restaurado. Un parche H15 alternativo, más pequeño, no debe sustituir H15 consolidado ni H16R. Los archivos RTP `michi_audio.c`, gestor `michi_session.c` y `test_michi_session.c` se comprobaron contra los paquetes H15/H16R y quedaron idénticos. Se limpió el staging temporal erróneo. Se preservó el `CURRENT_PRETEST_BASELINE.md` que identifica H16R. El generador H17 NO es un nuevo baseline de firmware.

## Matriz de ingeniería de fuente

| Sistema | Evidencia revisada | Pendiente legítimo de Drive | Condición estática de cierre |
|---|---|---|---|
| RTP/UDP | H15 `michi_audio.c`; PT97, IP/SSRC, 1920 B | Test del **motor real**, no solamente `michi_audio_fake.c` | Compilación de la fuente real en el harness host con inyección determinista de sockets y scheduler |
| PAUSE/RESUME | Helper `michi_audio_pause_policy.h`, test separado H15 | Regresión real `recvfrom` bloqueado + PAUSE + PCM in-flight | Receptor no se autodestruye ante pausa válida; error I2S genuino continúa terminal |
| RTP task lifecycle | H14 publish-before-worker, H15 transición serializada | Worker temprano/late exit, XTask creation fail, STOP concurrente | Ownership de socket, buffers y task handle inequívoco en cada camino |
| Jitter buffer | H16R restauró `michi_jb.c` de un reemplazo cruzado | Guardar integridad de papel del archivo y límites de payload | Source-role guard y tests de secuencias/payload conectados al Makefile |
| I2S | H8-H14 DMA, ring, quiesce, referencias de lifecycle | Intercalación de STOP/RESUME/DEINIT y cierre de recursos | Fuente C, shims y pruebas de falla cumplen una máquina de estados explícita |
| Sesiones | H16R `michi_session.c`, H15 rollback y zombie cleanup | Semántica READY/BUFFERING/PLAYING y error de salida separado | La API no describe audio audible sin señal medible; el token desaparece si el RTP murió |
| Lease/heartbeat | H16R watchdog autónomo y generación atómica | Evaluar TICK config y rotación; fallos combinados con queue full | El código tiene caducidad autónoma, incluyendo cuando falla la señalización por cola |
| Wi-Fi | Modo station y factory Wi-Fi preprovisionado | Configurable policy WPA2/WPA3 y mapeo de errores | No degradar de forma no intencional a redes abiertas; estados diagnósticos precisos |
| Michi Link discovery | mDNS `_michi-link._tcp.local` y Whisker firmado | Validadores de clock/firma/discovery y logs sensibles | Anuncios no mienten sobre confianza ni IP/tiempo |
| Home identity | `michi_home`, `michi_auth`, factory NVS | Tabla de transacciones y fallos (no borrado de Home) | No comprometer identidad ante error de flash, revocación o reprovisión |
| OTA | A/B y rollback con selftest PASS/DEGRADED | Correlación DEGRADED con SKU audio requerido | Política de aceptación coherente con `app_main.c` y tests de decisión |
| HTTP/API | `http_server.c` y schemas v1-lite | Consistencia del diagnóstico RTP/I2S y errores | API preserva contrato y no anuncia features experimentales sin servidor productor |
| UI del receptor | pantalla 320×240, assets y tests host | Estados de buffering/error con diagramas de paridad | UI refleja la fuente de verdad, sin suponer analog signal a partir de RUNNING |
| Simulator | Flask, E2E de contratos, RTP UDP sintético | Distinguir ejecución de motor real y recepción del socket simulado | Tests E2E no certifican PCM físico; escenarios cubren fallos de control |
| Host tests | Makefile C tests, `run_tests.sh` | Harness del RTP real y fault injections faltantes | Tests fuente reales conectados a runner y CI sin falsas exenciones |
| CI | `.github/workflows/ci.yml` | Integrar todos los nuevos objetivos y checks de integridad | Jobs no ejecutan binarios generados que pudieran estar viejos |
| Reproducibilidad | Manifiestos H14-H16R, H16 source-role guard | Snapshot integral y hash de cada archivo | Un único archivo de fuentes, fuera de repo, excluyendo `.git`, caches y secretos |
| Documentación | README/ROADMAP/H16R baseline | Guía canónica y retiro de informes incompatibles | Un solo baseline vigente, histórico correctamente etiquetado |

## H17 — mejora efectivamente implementada

Se creó `scripts/package_source_snapshot.py` (fuente Python): genera un ZIP reproducible con timestamps y permisos constantes, manifiesto `SOURCE_SNAPSHOT_MANIFEST.json` con SHA-256 por archivo, verifica todas las entradas antes de publicación y no sobreescribe salidas preexistentes. No incluye `.git`, compilaciones, caches, `sdkconfig` específico de máquina ni artefactos de firmware flasheable; detecta sin seguir symlinks y nombres de claves privadas evidentes. Su propósito es **source handoff**; no construye firmware ni certifica dispositivos.

Se creó `tests/test_source_snapshot.py`, con casos de determinismo, exclusiones, rechazos de symlink/material sensible y protección contra overwrite. El runner `scripts/run_tests.sh` ahora incluye la suite `source snapshot packaging contracts`; la ejecución será exclusivamente local.

**Advertencia de alcance:** el script aún debe examinarse sobre una descarga íntegra del proyecto; si algunos assets binarios son fuente indispensable, habrá que agregarlos mediante allowlist explícita y recalcular el manifiesto. No declarar completa la distribución real hasta generar el archivo desde el árbol entero. Los tests de la herramienta fueron escritos, **no ejecutados**.

## Brechas bloqueantes para el 100% de código Drive

### P0 — Test directo de `michi_audio.c`

`tests/host/test_michi_session.c` utiliza fake RTP. Debe crearse el harness que enlace `michi_audio.c` real contra shims deterministas de socket, PSRAM, scheduler, I2S y clock, con escenarios de PAUSE/STOP tardíos, workers que terminan prematuramente, falla de creación, falta de memoria, RTP malformado y limpieza de recursos. Su mera existencia en carpeta no es suficiente: el objetivo debe estar conectado al Makefile y CI.

### P1 — Contrato y diagnóstico de reproducción

`post_started_chain()` publica `PLAYING` cuando existe una tarea, antes de demostrar entrega PCM. Debe distinguir intención de reproducir, sesión establecida, buffering, recepción RTP y entrega I2S en el modelo de diagnóstico, preservando el schema v1-lite (extensiones opcionales y feature flags con default seguro, nunca cambiar enums unilateralmente). Evitar promesas de silencio analógico sample-clean.

### P1 — Robustez de seguridad y falla de hardware

Faltan tablas de errores exhaustivas y tests de recuperación de credenciales/factory provisioning, políticas Wi-Fi y OTA según SKU; no modificar contratos Home/Whisker/RTP sin revisar el productor Micro Server y el emisor Ecosystem. Toda modificación en Micro Server debe acompañarse de su WebUI.

### P1 — Empaquetado y reconciliación total

Aunque la herramienta fuente H17 ya está escrita, no hay evidencia de un snapshot integral construido desde *todos* los archivos originales de la carpeta Drive. Los ZIP históricos no son la distribución final. Se debe reconciliar README, ROADMAP, CI, scripts y carpeta raíz antes de declararlo cerrado.

## Entrega local independiente del porcentaje Drive

Después de alcanzar 100% fuente: descargar la carpeta Drive, conservar `.git` local, validar integridad y ejecutar todo con OpenCode/Antigravity; compilar ESP-IDF, ejecutar tests C/Python/E2E, corregir fallos descubiertos, probar ESP32-S3 + PCM5122, detectar límites de DMA, jitter y pérdida, ejecutar validación de OTA/reflash segura. Cualquier fallo abre una tarea nueva, **no convierte la preparación previa en PASS retroactivo**.

## Veredicto

H16R + herramientas H17 avanzan el cierre. Existen P0/P1 fuente pendientes, en especial el harness del motor RTP real y la semántica de la reproducción, por lo que **no se certifica 100% de trabajo Drive**. La integridad del archivo y el éxito de escritura en Drive son hechos verificables; la corrección funcional requiere evidencias posteriores.

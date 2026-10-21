# Michi Music Stream — puertas de cierre 100% de trabajo de fuente en Drive
Fecha: 2026-10-19. Autoridad actual: **H15-PRETEST**, fuentes modificados en sus rutas originales. No confundir con pruebas aprobadas.

## Hallazgo de control de versiones
La copia original de Google Drive ya contiene H15 y su paquete acumulativo completo. Un parche de revisión H15 alternativo, preparado posteriormente, cubre solo tres archivos y NO debe sobreponerse al H15 consolidado sin revisión línea por línea. Sus contenidos se solapan con correcciones más extensas ya presentes en el informe H15.

## Puertas pendientes para cerrar 100% de ingeniería preparable en Drive

### Gate 1 — motor RTP real (bloqueante)
Crear test host de `firmware/components/michi_audio/michi_audio.c` real, enlazado desde `tests/host/Makefile`; no sustituir por fake `michi_audio.c`. Shims de LwIP/socket, PSRAM, task scheduler y salida I2S con inyección de fallos. Casos deterministas: PAUSE durante recvfrom, STOP durante escritura PCM, error bind/alloc, fallo xTaskCreate, tarea que concluye durante publicación, autoconclusión por socket, timeout y nuevo START, SSRC e IP inválidos. Comprobar sin filtrado de fallos.

### Gate 2 — temporizador/autonomía (bloqueante)
Prueba determinista de cola de watchdog saturada junto con fallo de `esp_timer_restart` y ausencia de llamadas HTTP. El lease no puede quedar abierto indefinidamente si falla ambas rutas. Diseñar solución autónoma fail-closed sin bloquear callback `esp_timer`; demostrar que no reutiliza generación obsoleta.

### Gate 3 — máquina de estados y audio veraz
Separar sesión abierta, recepción RTP, buffering, pausa, PCM entregado e I2S fault en diagnóstico sin cambiar esquemas v1-lite unilateralmente. Garantizar que cualquier cierre parcial invalida el token al desaparecer transporte, preserva error de hardware, permite recuperación sin pérdida de Home.

### Gate 4 — factory/Home/Wi-Fi/OTA
Auditar exhaustivamente rutas de entrada y fallo en autenticación, revocación, material sensible, NVS y configuración factory. Aceptación de OTA en DEGRADED debe concordar con disponibilidad del audio obligatorio de la SKU. Agregar pruebas fuente de transacciones/rechazos, y documentar política de redes abiertas.

### Gate 5 — paquete integral limpio
Generar snapshot completo de fuentes, herramientas, contratos y tests DESDE los archivos actuales de Drive, preservando carpetas relevantes; excluir `.git`, builds, caches, ejecutables y credenciales privadas. Manifestar sha256 por archivo y versión del contrato, crear único ZIP final, actualizar README/ROADMAP/baseline y pasar lista de integridad. No depender de superponer cinco ZIP históricos. Conservar carpeta original y no eliminar staging ajeno sin autorización.

## No forma parte del porcentaje Drive
Ejecución local de tests y CI, compilación ESP-IDF, dispositivo ESP32-S3 y DAC, pruebas de estrés o 24 horas, prueba de Micro Server/WebUI, cambios en repos externos.

## Reglas de aceptación
No marcar el gate aprobado por solo añadir documentación o tests no conectados. Un cierre estático al 100% requiere todos los problemas fuente conocidos resueltos y pruebas preparadas, no necesariamente ejecutadas. Si no está toda la fuente disponible o existen cambios concurrentes no reconciliados, declarar bloqueo y no sobrescribir. La validación posterior podría detectar defectos nuevos.

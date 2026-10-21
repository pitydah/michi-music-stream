# OpenCode / Antigravity — validación y cierre local H13 Michi Music Stream

## Objetivo

Validar el árbol real de Michi Music Stream descargado desde Google Drive y corregir lo que impida un build/reproducción confiables. H13 introduce atomicidad RTP, un lease de sesión y un contador de referencias del canal de control I2S. Conservar contratos congelados y todos los avances H1–H12. Trabajo sobre rama principal solo si el propietario así lo dispone; nunca reescribir cambios no relacionados.

## Antes de ejecutar pruebas

1. Respaldar repo y preservar `.git`; descargar la carpeta íntegra Drive.
2. Extraer `docs/MICHI_MUSIC_STREAM_H13_PRETEST_CONSOLIDATED.zip` en raíz para asegurarse de aplicar el paquete acumulativo, sin sustituir `.git`. Revisar `docs/H13_SOURCE_MANIFEST.json` SHA-256, `git status`, diff y compatibilidad de archivos de Google Drive.
3. Confirmar versión ESP-IDF (objetivo v5.3), git HEAD y bundle vendorizado Michi Link exacto. No ejecutar scripts destructivos de flasheo ni sobrescribir identidades.

## Puertas obligatorias

G0. Inspección con `git diff --check`, rutas, imports y dependencias ESP-IDF. Confirmar `_Atomic(TaskHandle_t)` en toolchain Xtensa, semántica de `portMUX` y C11. Reportar warnings / errores.
G1. Ejecutar `make -C tests/host clean && make -C tests/host test`, con salida completa y libcjson disponible. Foco `test_h13_control_reference_blocks_deinit`, carreras H11/H12, command timeouts, STOP/START/DEINIT, FAULTED y DMA short.
G2. Ejecutar `./scripts/run_tests.sh --full`, CI y build `cd firmware && idf.py build` tras limpieza controlada de caches. Ninguna suite omitida se cuenta como PASS.
G3. Revisar intercalaciones concurrentes START/STOP/PAUSE y self-ended RTP task; probar inicialización prematura, done antes de publicación de handle, STOP tras fallo de QUIESCE, recuperación FAULTED, dos callers simultáneos, timer wrap, socket receive error.
G4. Añadir/ajustar **pruebas dinámicas del motor RTP real**: no basta con inspección estática de `_Atomic`. Simular soporte de tarea/socket cuando sea viable; pruebas E2E con backend y sesión honesta. Detectar deadlocks, uso de handles liberados y regresiones de audio.
G5. Comparar contrato RTP 48 kHz S16LE 10 ms PT97/SSRC con Micro Server y Michi Link; verificar reensamblado IPv4 real bajo fragmentación y pérdida, no alterar solo Stream.
G6. Verificar seguridad Home, BLE disabled production, factory provisioning, conexión IP/mDNS/Whisker y diagnósticos WebUI. Comprobar no se anuncia `now_playing` antes de soporte real.
G7. Solo con autorización explícita, pruebas físicas PCM5122 ESP32-S3: primeras sesiones, mute/unmute, reconexión, pérdida Wi-Fi, fallo de alimentación OTA A/B, rollback e identidad recuperable, reproducción 24 h y logs/mediciones.

## Informe final obligatorio

Tabla PASS/FAIL/BLOCKED por cada puerta, command exacto, dispositivo y firmware SHA, tests contados, CI run link, defectos P0/P1/P2, reparaciones y diff. No afirmar "100%" si un P0 no está cerrado o falta hardware. No abrir PR ni hacer push sin política explícita del propietario.

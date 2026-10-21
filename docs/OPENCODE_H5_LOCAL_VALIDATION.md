# OpenCode — validación local de H5 (H4 + H5 acumulativos)

## Objetivo

Descargar la carpeta de Drive con H4+H5 ya integrado (el ZIP se conserva como respaldo alternativo), verificando el diff contra tu checkout local; conservar `.git`, no reemplazarlo. No mezclar modificaciones recientes de OpenCode; conciliar conflictos antes de reemplazar. **No hagas push si alguna prueba falla.**

## Acciones

1. Copiar las fuentes H5 descargadas de Drive dentro del repositorio conservando las rutas (o usar el ZIP como alternativa, nunca ambas rutas sin comparar) y crear un diff `git status --short` / `git diff --check`. Comparar con H3 y asegurar que no se perdieron correcciones previas de `michi_audio.c` / `michi_jb.c`.
2. Ejecutar `./scripts/run_tests.sh --full`; explicar cada fallo, no reportar verdes ficticios. Verificar `tests/host/test_michi_jb.c` con la nueva cabecera portable `michi_audio_recv_policy.h`.
3. Compilar ESP-IDF para ESP32-S3 en ambiente real, `idf.py set-target esp32s3 && idf.py build`; revisar advertencias y memoria PSRAM y heap interno.
4. Probar: secuencias RTP fuera de orden, wrap 65535→0, payload 1920, 1921 (drop), 0, duplicados y pérdida de paquetes. Simular UDP socket error persistente si hay shim/inyección; comprobar cierre por tres errores inesperados, pero NO por EAGAIN/EWOULDBLOCK/EINTR.
5. Verificar que recovery de underrun no retrocede playhead, que falla `write_silence()` y se reporta estado terminal sin doble liberación o fuga. Revisar `session_stop()` concurrente, especialmente salida autónoma y `s_session_done`.
6. Flashear en placa ESP32-S3 con PCM5122, comparar memoria y latencia H3/H5 y mantener reproducción 24h. Recoger logs, métricas y estado HTTP.
7. Repetir pruebas Wi-Fi cortado, reinicio Micro Server, pausa/reanudación y OTA conservadora. No editar protocolos MICHI-F1, Home o Michi Link en este sprint.
8. Corregir en local cualquier fallo encontrado, registrar SHA, resultados y un informe `H5_VALIDATION_RESULT.md` con verdict `PASS/FAIL/BLOCKED` por caso; actualizar porcentaje solo con evidencia.

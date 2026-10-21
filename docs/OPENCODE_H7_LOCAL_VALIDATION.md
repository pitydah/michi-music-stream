# OpenCode — Validación local H7 (sin ejecutar en ChatGPT)

Trabaja sobre la copia local de `michi-music-stream` después de descargar Google Drive. Conserva el directorio `.git` original y verifica los cambios contra HEAD (`git status`, `git diff --check`, `git diff --stat`); no sustituyas `.git` con el respaldo de Drive.

## 1. Compilación y tests host

1. Revisar `michi_volume.c`, `michi_audio.c`, `michi_session.c` y la nueva API en `michi_volume.h` por compilación C11/ESP-IDF y sincronización multicore.
2. `cd tests/host && make clean && make test_michi_volume && ./test_michi_volume`.
3. `make test_michi_session test_michi_audio_output && ./test_michi_session && ./test_michi_audio_output`.
4. Ejecutar `../../scripts/run_tests.sh --full`, registrar nombre exacto de cada suite, exit codes y dependencias. No omitir cJSON ni los tests de audio.
5. ESP-IDF: configurar target `esp32s3`, compilar firmware limpio y guardar errores/warnings, tamaños de memoria y análisis del mapa.

## 2. Pruebas específicas

- Mute TRUE sobre software-volume 65: salida digital toda cero, nivel configurado permanece 65.
- Establecer volumen 35 estando muteado: sigue silencio; mute FALSE restaura 35.
- Mute TRUE con DAC hardware-volume activo: bloque de PCM queda cero, ningún write hardware destructivo cambia el nivel DAC.
- DAC hardware write fallido: fallback digital; revisar si atenuación hardware acumulada exige estado DEGRADED/ERROR.
- Simular 1000 ciclos de mute/unmute y volume-set concurrentes; revisar audio, estado y ausencia de data races.
- Intercalar PATCH(PAUSE/RESUME) con STOP/START y expiration de lease; verificar que ningún evento antiguo cambie el estado de nueva sesión.
- En placa real, comprobar mute/unmute y volumen a niveles bajos, medios y altos; observar clics/pops y que los cambios no provoquen rebuffer.

## 3. Negativos/no regresión

Prohibido modificar Michi Link, Ecosystem, Micro Server o cambiar PCM/RTP, Home, OTA. Comprobar que `GET diagnostics`, sessions, watchdog, receptor mDNS/Whisker y reproducción no anuncien capacidades inexistentes.

## 4. Reporte final

Entregar: SHA local, diff de archivos, resultados exactos, suite host, ESP-IDF, CI, hardware si disponible, regresiones encontradas y correcciones. Si falla un test no marcar H7 cerrado: corregir hasta verde y reproducir. Comparar contra baseline H6.

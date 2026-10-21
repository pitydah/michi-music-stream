# Prompt para OpenCode — H8 Michi Music Stream: validación local

Trabaja exclusivamente en el repositorio local Michi Music Stream que acaba de ser actualizado desde la carpeta de Google Drive. NO implementar funciones extra ni modificar Michi Link, Micro Server, Ecosystem, Home, OTA o el contrato RTP. Antes de probar, revisa `git diff` y conserva el directorio `.git` local. Código H8 no fue compilado ni probado por ChatGPT.

## Objetivo bloqueante

1. Revisar la coherencia de `michi_volume_configure_pcm`, `michi_volume_reset_envelope` y `michi_volume_apply` con todos sus consumidores y mocks. Verificar compilación C11 real tanto con Xtensa/ESP-IDF como en host. Confirmar que `test_michi_volume`, `test_michi_audio_output` y `test_michi_session` enlazan (especialmente `shim/michi_volume_fake.c`). Resolver faltantes sin esconder warnings.
2. Ejecutar `make -C tests/host test` y la matriz host/CI existente. Exigir cero fallos y anotar lo no ejecutado. Compilar firmware con `idf.py build` para la placa ESP32-S3 correcta; registrar el target, versiones, SDK y SHA.
3. Inspeccionar la rampa Q27 para overflow, off-by-one, ganancias negativas, 16-bit endian, S24LE sign extension, cero exacto en mute tras 10 ms y re-target durante una rampa. Probar 8k, 44.1k, 48k, 96k y 1/2 canales en host; `michi_volume_apply` debe ser llamado por un solo consumidor. Nunca introducir asignaciones de heap en esa ruta.
4. Verificar que cada `michi_audio_output_write()` publicará solo fotogramas enteros al ring SPSC; cubrir wrapped indices con S24 stereo, capacidades cerca de llenado, entradas 0/3/5/6/1920 B y stop/quiesce/resume concurrentes. Si aparece bloqueo por ring lleno, informar sin ocultarlo.
5. Comprobar que el DAC con volumen hardware no recibe una segunda atenuación digital tras asentarse el fade. La rampa de mute debe actuar en todas las rutas, pero los cambios de volumen directamente en el DAC no cuentan como suavizados. No anunciar certificación acústica si no hubo pruebas físicas.
6. Revisar especialmente race conditions con `michi_volume_init` y `configure_pcm` durante STOP/START, fallos de `i2s_channel_enable`, cambios rápidos de mute/volumen y niveles de prefill. No ejecutar funciones del ciclo de vida fuera del contrato de single-consumer.

## Informes exigidos

- SHA antes/después; lista de archivos y cambios introducidos al reparar regresiones.
- Comandos y resultados completos de `make test`, compilación ESP-IDF y CI (PASS/FAIL/SKIP reales).
- Host fault injection: buffer lleno, I2S write fail, tarea detenida, PCM parcial, retarget en medio del fade.
- Pruebas físicas independientes: clicks/pops al empezar/parar/mute/unmute, rampa por 10 ms, amplitud 16-bit, PCM5122 I2C, 24-bit solo si hardware y contrato permiten; sin fingir ensayos.
- Revertir H8 de forma atómica si la compilación revela un problema fundamental; nunca eliminar H7 ni alterar las credenciales de Home.

# Michi Music Stream — H8: rampa PCM y integridad de fotogramas I2S

**Base:** H7 de Google Drive, 19 de octubre de 2026. **Estado:** código fuente y regresiones escritos; no se han ejecutado compilaciones, pruebas ni certificación acústica/hardware. Sin cambios en Michi Link, Ecosystem, Micro Server, Home, OTA ni contrato RTP v1-lite.

## Defectos detectados

1. **H8-01 (P1, transitorios):** H7 pasaba de PCM pleno a PCM cero al activar mute, y de cero a pleno al desmutear. La discontinuidad puede provocar clics audibles. También los cambios de ganancia digital se aplicaban en escalón entre buffers, sin rampa.
2. **H8-02 (P1, formato):** `michi_audio_output_write()` aceptaba longitudes arbitrarias de PCM; `ring_write()` podía publicar una porción de fotograma si quedaba espacio no alineado. En estéreo S16LE cada fotograma son 4 bytes; en estéreo 24-bit packed son 6 bytes. El consumidor I2S y la ganancia no deben recibir fotogramas partidos.
3. **H8-03 (P1, reinicio):** tras STOP/START o QUIESCE/RESUME el estado de la envolvente digital podría conservar el último nivel, con salto al llegar audio nuevo. La rampa se reinicia únicamente en puntos seguros del ciclo de vida.

## Cambios de firmware

- `michi_volume.c/.h`: envolvente lineal de **10 ms** basada en la frecuencia PCM real (8–96 kHz). La tarea I2S es el único escritor de la envolvente; los controles de volumen/mute conservan `_Atomic`. El estado Q27 y la pendiente se calculan sin heap ni divisiones por fotograma; cada fotograma intercalado aplica la misma ganancia a todos los canales.
- El mute sigue siendo independiente del volumen configurado, pero **el PCM tarda hasta 10 ms en silenciarse completamente**. `michi_volume_get()` no se convierte en una medida de nivel físico; expresa el valor configurado. La señal de mute lógica se actualiza inmediatamente.
- En ruta con volumen hardware, la rampa digital actúa sobre mute, arranque y reanudación. Los cambios de volumen ejecutados directamente por el DAC **no** son suavizados por este firmware y requieren medición física.
- `michi_audio_output.c`: fija `sample_rate` y `channels` al inicializar el consumidor, reinicia el sobre de ganancia antes de iniciar la tarea y desde la propia tarea al reanudar. Rechaza escrituras no múltiplo del tamaño de fotograma PCM. `ring_write()` no publica un fotograma incompleto; el ring mantiene los límites incluso con wrap y 24 bits empaquetados.
- Muestras 16-bit se escalan en enteros con `memcpy` para evitar aliasing de punteros sin alineación; 24-bit packed recupera signo correctamente sin shift de valores negativos. Los bytes sobrantes de un buffer de entrada irregular se silencian defensivamente.
- El stub `tests/host/shim/michi_volume_fake.c` recibe los nuevos símbolos para que `test_michi_audio_output` y `test_michi_session` continúen enlazando contra el stub host, no contra dos implementaciones reales.

## Pruebas de regresión añadidas (NO ejecutadas)

- Fade-in a 48 kHz, dos canales con la misma ganancia por fotograma, incremento monótono y convergencia después de 480 fotogramas.
- Fade-out/fade-in mute/unmute; volumen digital al 30 % convergente.
- Selección de frecuencia real: 96 kHz requiere 960 fotogramas para 10 ms.
- PCM packed S24 negativo y preservación del signo.
- Parámetros inválidos de frecuencia/canales y tratamiento defensivo de fragmentos PCM.
- Rechazo de escrituras parciales 16-bit stereo (3 B) y 24-bit stereo (5 B); aceptación de fragmentos de 1920 B completos.
- Adaptación de las aserciones H7 que suponían mute inmediato: ahora verifican silencio al final del fade de 10 ms.

## Límites y prioridades restantes

- Esto reduce discontinuidades PCM, pero **no certifica ausencia de clics analógicos** en PCM5122 ni PCM5102A. Es necesario medir osciloscopio/DAC/auriculares y reinicios de I2S.
- El canal I2S sigue sin garantía de progreso ante el caso patológico de un productor perpetuamente bloqueado por un ring lleno; evaluar watchdog de backpressure con pruebas dirigidas antes de cambiar su semántica.
- Si falla una escritura de volumen hardware, el fallback digital no conoce la atenuación física residual; sigue siendo un problema de seguridad y diagnóstico de nivel por tratar.
- La rampa es lineal en amplitud, no en percepción; no hay compresor ni DSP nuevos. No se modifica RTP/PT/SSRC/48 kHz ni negociación.
- Pendientes grandes: descubrimiento e interoperabilidad real Micro Server, incorporación Home por Ecosystem, OTA con recuperación, pruebas Wi-Fi adversariales y certificación de audio de 24 horas.

## Aplicación local

Los archivos originales de Drive fueron actualizados si su verificación figura en el mensaje de entrega. Descargar el árbol con las rutas preservadas sobre el repositorio Git local sin reemplazar `.git`. Ejecutar validación según `docs/OPENCODE_H8_LOCAL_VALIDATION.md`. No elevar el porcentaje global de madurez exclusivamente porque el código está escrito.

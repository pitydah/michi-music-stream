# MICHI STREAM — SIGNATURE HI-FI UI-02

Estado: **implementado en código fuente; pruebas ESP32 pendientes en laboratorio**.
Fuente de diseño: SVG oficial `firmware/components/michi_display/assets/brand/michi_stream_official.svg`.
Placa: Waveshare ESP32-S3-LCD-2, LCD 320×240, RGB565, render por bandas 320×40.

## Principios y decisiones

1. **Marca oficial, no gato reconstruido.** `gen_michi_brand.py` convierte SVG a las variantes 40/72 px RGB565; se cargan desde flash y se dibujan únicamente las líneas visibles en la banda DMA. Un icono compacto monocromo (no logotipo alternativo) se usa para los elementos pequeños.
2. **Tipografía premium viable.** Regeneración determinística de Noto Sans 2-bit (SM/MD/LG); `sdkconfig.defaults` habilita el mismo modo. Las fuentes originales se mantienen intactas y bajo su licencia, y el panel sigue sin framebuffer completo.
3. **Estética de producto.** Fondo `#080A0F`, paneles sólidos, acento cobre oficial `MICHI_UI_BRAND_COPPER`; superficies con bordes de 1 px. Se evita imitación costosa de cristal/transparencia.
4. **Estados verídicos.** Sin sesión: nunca afirmar audio negociado. IP, SNTP, mDNS y Whisker tienen indicadores distintos; el envío multicast no equivale a descubrimiento por Micro Server.
5. **Información en 2 pasos.** Vista Ready: indicadores de IP, SNTP, mDNS y Whisker siempre visibles. Vista Diagnostics: red y descubrimiento a la izquierda, sesión RTP/DAC/audio/volumen/PSRAM a la derecha.
6. **Sin pairing físico.** La UI de instalación remite a Michi Ecosystem por USB. Los estados auxiliares de pairing conservados temporalmente en la API interna son solo compatibilidad con tests históricos; nunca exponerlos como flujo activo. Eliminarlos en un cambio independiente de contrato/FSM.
7. **Protección del audio.** Sin heap durante render, sin cómputo SVG en runtime, sin widgets temporizados nuevos ni polling desde el hilo de dibujo; sigue el callback por bandas existente.

## Diagnóstico y ensayos físicos

Activar en firmware de laboratorio `CONFIG_MICHI_DISPLAY_DIAGNOSTICS_ON_BOOT=y` en `idf.py menuconfig` / configuración de compilación. **Mantener `n` en builds de producción.** Permite usar la pantalla para verificar el dispositivo sin un botón de pairing inexistente.

| Etapa | Evidencia local en pantalla | Lo que NO demuestra |
|---|---|---|
| Wi-Fi | enlace, RSSI y IPv4 | conexión con Micro Server |
| SNTP | reloj sincronizado | autoridad de Michi Home |
| mDNS | servicio iniciado | que el servidor vea el anuncio |
| Whisker | `TX firmados: N` en la sesión | que otro nodo reciba, verifique o acepte firma |
| Sesión RTP | activa / sin sesión | llegada continua de audio sin underruns |
| DAC | detección real/ausencia/error | bit-perfect ni medición analógica |
| Audio | formato validado de la sesión | formato del archivo original, salvo telemetría |
| PSRAM | medido o sin medición | tamaño declarado por la configuración de placa |

## Experiencias cubiertas

Boot, unprovisioned, connecting, provisioning, ready online/offline, session pending, buffering, playing con UTF-8 y títulos largos, paused, OTA porcentaje/desconocido, error recuperable/fatal, diagnósticos y volumen. Las capturas PNG bajo `docs/ui/previews/` son simulaciones del motor de firmware, no fotografías ni certificación física.

## Validación local (OpenCode)

1. Comparar las fuentes modificadas con original limpio: `git diff --check` y revisar el `git diff` manualmente (copia Drive tiene `.git` que puede divergir de fuentes editadas).
2. Regenerar ambos activos: `cd firmware/components/michi_display && python3 tools/gen_michi_ui_assets.py --raster 2bit --strict && python3 tools/gen_michi_brand.py`. Verificar hashes/diff esperados y fuentes Noto válidas.
3. Compilar los host tests de UI: `make -C tests/host clean test_michi_ui` y ejecutar `./tests/host/test_michi_ui`, más prueba de DMA. Si falla una fixture histórica, corregir la fixture sin reintroducir pairing HTTP.
4. Renderizar los 26 escenarios de preview; comparar geometría/recortes, diferencia entre estados, multibanda y textos españoles; no sobrescribir automáticamente imágenes sin inspección visual.
5. Build ESP-IDF 5.3 real sobre `esp32s3`. Verificar flash del sprite, font coverage, tamaños de stack, DMA banding, ciclo de vida OTA, consumo CPU y ninguna regresión del audio I2S.
6. En hardware de laboratorio activar diagnóstico al arranque. Fotografiar IP, SNTP, mDNS, TX firmados, sesión, DAC y audio, y confrontarlos con logs de Micro Server/WebUI. No certificar DEVICE_E2E_PASS sin esta prueba.

### Bloqueos / decisiones pendientes

- Acceso operativo al menú de diagnóstico desde Ecosystem/servidor no está implementado: modo de laboratorio por Kconfig cubre pruebas, pero una futura acción remota segura deberá tener autorización Michi Home.
- `michi_display` no recibe todavía jitter/underruns, validación de firma remota ni latencia medida; no inventar estos indicadores.
- Revisar la ruta de pairing histórica en FSM y host tests en sprint de retiro independiente.
- Si el icono oficial se cambia, regenerar activos desde la fuente SVG y revisar ópticamente las variantes pequeñas; nunca editar la marca manualmente.

## Notas de integridad de la copia de Drive

Los cambios en archivos protegidos por el conector pueden quedar pendientes.
El paquete fuente de la UI debe usarse solo tras cotejar que `michi_ui_screens.c`,
`michi_ui_icons.c`, `michi_ui_icons.h`, `michi_stream_brand_data.h`
y `michi_ui_fonts_data.h` están presentes y alineados. Si el generador de
iconos continúa en su versión anterior, **no dar por cerrada su regeneración
determinista**; hasta corregirla, utilizar los activos generados guardados.
Las capturas nuevas son evidencia de render host, no una prueba física.

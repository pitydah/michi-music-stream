# Michi Music Stream H16R — recuperación de código e integridad del árbol

## Hallazgo grave
El archivo Drive `firmware/components/michi_audio/michi_jb.c` (ID `1NNZh96sUXv4_tt_OPGLa4kBov_8YaEpb`) fue sobrescrito previamente por el código de `michi_http/http_server.c` de 73.947 bytes. La inspección del encabezado (`HTTP API layer`) y las funciones de servidor confirma contaminación cruzada, no una refactorización válida. Su revisión anterior de Drive era de 7.085 bytes.

## Recuperación aplicada
Se restauró directamente en el archivo original una versión fuente de 7.324 bytes llamada `michi_jb_recovery.c`, conservada en el staging H15 original. Contiene la implementación real de `michi_jb_*`, incluida comprobación de payload excedente antes de desalojar paquetes, política de playout y liberación de slots. SHA-256: `974bc8082bb7d816ac3892ac756b85408bee02226c9a8adc0b06c29d3fe302e7`.

## Prevención permanente
Nuevo `scripts/check_source_identity.py` con verificación de firmas de implementación y ausencia de marcadores de otros componentes para: jitter buffer, servidor HTTP, RTP, salida I2S y gestor de sesión. El runner `scripts/run_tests.sh` ejecuta esta puerta ANTES de las suites y GitHub CI la ejecuta inmediatamente después de checkout.

La inspección de rol es una heurística estática explícita: detecta la sustitución cruzada ocurrida, pero no equivale a compilación ni a pruebas de comportamiento. El desarrollador debe inspeccionar avisos de nuevas rutas y actualizar firmas cuidadosamente cuando cambie la arquitectura.

## Baseline y estado
H16R conserva los seis cambios H16 de watchdog sobre H15, agrega recuperación JB y guardia estructural. No modifica RTP PT97/1920B, Home, firma, DAC ni interfaces públicas. Compilación/CI/tests/físico siguen SIN EJECUTAR. Persisten como trabajo de Drive un harness del motor RTP real, endurecimiento de seguridad y distribución integral limpia.

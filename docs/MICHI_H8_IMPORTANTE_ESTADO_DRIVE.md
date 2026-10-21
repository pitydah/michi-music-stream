# Michi Music Stream — H8: estado de entrega e instrucciones

Fecha: 2026-10-19. **AVISO: la carpeta Drive original ha recibido una actualización PARCIAL** debido a bloqueos intermitentes del conector al sobrescribir archivos grandes. No utilizar la carpeta original por sí sola como una versión H8 validable. No se han ejecutado compilaciones o pruebas.

## Estado confirmado por metadatos de Drive

- `firmware/components/michi_volume/michi_volume.c`: nuevo H8, 8178 bytes (escritura confirmada).
- `firmware/components/michi_volume/include/michi_volume.h`: nuevo H8, 4240 bytes (escritura confirmada).
- `firmware/components/michi_audio_output/michi_audio_output.c`: todavía H7, 38739 bytes (sobrescritura rechazada).
- `tests/host/shim/michi_volume_fake.c`: todavía H7, 652 bytes.
- `tests/host/test_michi_volume.c`: todavía H7, 4734 bytes.
- `tests/host/test_michi_audio_output.c`: todavía H7, 44679 bytes.

Se intentó restaurar la versión H7 del módulo de volumen para conservar consistencia, pero el conector también rechazó esa escritura. **La versión original en Drive no es un conjunto H8 consistente.**

## Versión H8 completa y forma segura de trabajar

El archivo **`docs/MICHI_MUSIC_STREAM_H8_FULL.zip`** contiene *todos los archivos nuevos H8*, con sus rutas relativas correctas y documentación. Para validar H8:

1. Conservar el repositorio Git local y el directorio `.git`.
2. Descargar la carpeta de Drive y, antes de compilar, **extraer el ZIP H8 en la raíz** de esa copia, sobrescribiendo los archivos que contiene. Alternativamente, restaurar H7 íntegro desde el historial local si no se desea aplicar H8 todavía.
3. Revisar `git diff` y ejecutar el prompt `docs/OPENCODE_H8_LOCAL_VALIDATION.md` con OpenCode en el equipo local.
4. No flashear la placa antes de compilar y ejecutar las regresiones de volumen e I2S.

El ZIP sirve como parche acumulativo de H8 sobre H7. No modifica Michi Link, Home, la red ni OTA. La rampa de ganancia digital de 10 ms no constituye certificación de ausencia de clics físicos.

## Cierre administrativo

Hasta que se consigan actualizar los seis archivos originales de Drive como una unidad, el estado de H8 es **entregado como paquete verificable para validación local, sin integración completa en la carpeta original**. No sumar progreso certificado a la madurez global solo por esta entrega.

# Michi Music Stream — Estado del cierre integral de fuentes

Fecha: 20 de octubre de 2026. Alcance: trabajo de preparación de fuentes en Google Drive, separado de compilación, pruebas y hardware.

## Verificado en la copia de Google Drive

- Carpeta de proyecto original identificada: `1joizlg0NYm4wz4T3gNFijF_tKMpbnMvD`.
- Estructura raíz identificada: firmware, contratos Michi Link, scripts, simulador, tests, docs, ejemplos y CI.
- `firmware/components/` contiene 19 directorios de componentes: michi_audio, michi_audio_output, michi_board, michi_dac, michi_discovery, michi_display, michi_home, michi_http, michi_identity, michi_led, michi_log, michi_ota, michi_product_profile, michi_sd, michi_session, michi_state, michi_time, michi_volume y michi_wifi.
- El generador de snapshot de Drive se había reconciliado con H19 durante H21. No requiere aplicar los ZIP históricos.
- La carpeta contiene artefactos que no deben formar parte de la entrega fuente: `.git`, `firmware/build`, `firmware/managed_components`, `.pytest_cache` y ZIP históricos.

## Corrección preparada en esta pasada (NO sincronizada a Drive)

- `package_source_snapshot.py`: ahora exige que estén presentes los 19 componentes del firmware, las carpetas de tests host/contract/e2e, el workflow CI, la configuración de producción y las fuentes/pruebas H19–H20.
- La modificación se entregó en esta conversación como archivo local. La carga al origen de Drive falló por `container_session_expired` pese a un reintento. Debe sustituirse el archivo en Drive cuando esté disponible la transferencia.

## Por qué no se certifica el 100% de Drive

No existe aún un ZIP íntegro construido a partir de los bytes de todos los archivos de la carpeta actual. Las APIs disponibles permitieron enumerar su estructura, pero no proporcionaron una descarga integral del directorio. En consecuencia, no se ha podido ejecutar una comparación SHA-256 archivo a archivo de todo el proyecto contra un paquete íntegro. No es correcto declarar un snapshot completo ni estimar nuevos porcentajes sobre ese resultado incompleto.

## Criterio de aceptación

1. Descargar de Drive todos los archivos fuente del árbol actual, conservando las rutas relativas y excluyendo artefactos generados, credenciales y `.git`.
2. Incorporar el `package_source_snapshot.py` reforzado a `scripts/`.
3. Generar un ZIP integral de fuentes con `SOURCE_SNAPSHOT_MANIFEST.json` utilizando el generador; dejar ese ZIP accesible en el directorio de Drive.
4. Contrastar los SHA-256 del ZIP y del manifiesto contra las fuentes enumeradas; comparar el número total de archivos esperados con el número realmente exportado.
5. Publicar una sola referencia de release que remita al ZIP íntegro, sin requerir overlays H8–H20.

La compilación, tests locales y hardware son un proceso posterior independiente.

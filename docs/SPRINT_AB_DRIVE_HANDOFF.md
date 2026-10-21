# Michi Music Stream — Sprint A/B: entrega editable en Google Drive

## Alcance aplicado

- Nueva extension local `/api/v1/receiver-lite/now-playing` con bearer
  autenticado, token de sesion, id y controlador correctos, lease vigente,
  cuerpo JSON estricto de 4 campos y respuesta con confirmacion.
- `michi_session_set_now_playing` mantiene comprobacion de sesion bajo mutex y
  usa la API existente de visualizacion, sin modificar RTP ni DSP.
- `features.now_playing` continua **false** mientras la extension y el
  productor de Micro Server no hayan sido certificados; no inventar soporte.
- CI: recursos Noto Sans a 2 bpp y generacion reproducible de pictogramas
  UI-06; mantiene el diff de assets versionados.
- La UI integra en Drive la etiqueta de sala/fuente en la esquina superior
  derecha y la omite si no hay fuente, dejando el footer libre.
- README/OVERVIEW/ROADMAP alineados con el estado real y con
  `tests/host/test_michi_home.c`, ahora presente en Google Drive.

## Exclusiones deliberadas

- NO se altera paquetizacion RTP 10 ms / 1920 bytes: requiere contrato de
  ambos extremos y ensayo de MTU/fragmentacion.
- NO se quita BLE o rutas FSM heredadas en este sprint; una eliminacion sin
  ruta de recuperacion certificada podria inutilizar equipos ya flasheados.
- NO se modifica OTA sin prueba real de rollback y firmas.
- NO se promete descubrimiento mDNS/Whisker end-to-end ni audio validado:
  hace falta conexion con Micro Server y la WebUI, y placa fisica.

## Riesgos y criterios de promocion

El handler nuevo debe pasar pruebas negativas de todas las credenciales,
campos JSON y carreras de cierre; el endpoint es experimental y el flag
permanece desactivado. Los tests existentes de capacidades esperan false.
El pipeline UI requiere generacion identica a los headers de 2 bits y de
precision; si aparece drift, revisar entradas/versiones antes de regenerar
archivos por fuerza.

## Fuente de verdad

Archivos originales de la carpeta compartida de Drive, no los `.bin`,
`firmware/build` ni directorios `.pytest_cache`. Los ensayos fisicos y CI
son posteriores a la descarga por el usuario.

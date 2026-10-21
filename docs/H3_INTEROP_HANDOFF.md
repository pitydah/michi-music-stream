# Michi Music Stream — Sprint H3: interoperabilidad de metadatos y visibilidad remota

## Archivos modificados (copia original de Google Drive)

- `simulator/receiver_sim.py`: simulación de `now-playing`, propietario de
  sesión asociado a Bearer, revisión monótona, limpieza al cerrar/caducar.
- `simulator/tests/test_integration_http.py`: casos positivos y adversariales.
- `firmware/components/michi_session/include/michi_session.h`: añade el contador
  `metadata_revision` al snapshot interno de sesión.
- `firmware/components/michi_session/michi_session.c`: copia el contador bajo
  mutex al devolver el snapshot, sin exponer contenido musical.
- `firmware/components/michi_http/http_server.c`: expone
  `session.metadata_revision` en el diagnóstico protegido, solo con sesión activa.
- Documentación `NOW_PLAYING_EXTENSION.md`, `ROADMAP.md` y `simulator/README.md`.

## Diseño y límites

1. `features.now_playing` sigue en **false**, como exige el estado no certificado.
2. No se cambia `contracts/michi-link` ni el formato RTP 10 ms; no se anuncian
   codecs, tasa de muestreo ni Hi-Res inexistentes.
3. El simulador sirve como blanco determinista para validación de Micro Server;
   no reemplaza ESP32-S3, SPI/LCD, I2S/DAC ni pruebas de red real.
4. `metadata_revision` aporta confirmación de aplicación local de un cambio de
   pista, NO confirma que el receptor haya reproducido RTP o emitido audio.
5. El contrato necesita implementación del productor en Micro Server y
   homologación entre ambos repositorios antes de activar la capacidad.
6. El simulador aplica validaciones de protocolo en rutas nuevas; no se
   ha realizado una auditoría integral de carreras en todo el simulador.

## Integración pendiente — Micro Server/WebUI

- Implementar envío de metadatos con revisión `uint32` por sesión y orden
  de última revisión, autenticando con el mismo controlador propietario.
- Presentar `metadata_sent_revision` y `metadata_accepted_revision` (obtenida
  desde GET diagnostics del Stream) como valores distintos; nunca confundir
  con el estado de reproducción confirmado.
- Si el endpoint no está anunciado, usar flag de laboratorio, no habilitación
  automática. Gestionar 401/403, 404, 409, 415 y errores de red sin reintentos
  que reordenen canciones antiguas.
- Reconciliar cambios concurrentes de pista, reconexión Home y lease expirado.
- Medir en hardware envío de metadata, red Wi-Fi y carga LCD con audio.

## Validación

No se ejecutaron suites ni build de firmware en la etapa de edición de Drive.
Usar `docs/OPENCODE_H3_LOCAL_VALIDATION.md` tras descargar y aplicar el árbol
sobre la copia local Git; conservar un diff y reportar todo fallo.

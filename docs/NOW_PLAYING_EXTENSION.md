# Michi Music Stream — Now Playing experimental (Sprint A/B)

## Estado / autoridad

Este archivo describe exclusivamente la extension LOCAL implementada en Stream.
**No modifica** el bundle vendorizado Michi Link `1.0.0-alpha.5` ni proclama una
extension certificada por Micro Server. `features.now_playing=false` por diseno
hasta que se pruebe la integracion Stream+Micro Server y se publiquen pruebas
positivas de contrato. El endpoint implementado queda disponible en firmware
de laboratorio para pruebas autenticadas.

## Endpoint

`PUT /api/v1/receiver-lite/now-playing`

Cabeceras obligatorias:
- `Authorization: Bearer <token_de_autenticacion_Michi_Home>` con permiso `MICHI_PERM_PLAYBACK`.
- `X-Michi-Session: <token_de_la_sesion_RTP>` emitido al crear la sesion.
- `Content-Type: application/json`.

Cuerpo JSON obligatorio, exactamente cinco claves (cuatro `string` y `revision` entero positivo):

```json
{
  "session_id": "550e8400-e29b-41d4-a716-446655440000",
  "source": "Living Room",
  "title": "Teardrop",
  "artist": "Massive Attack",
  "revision": 1
}
```

Limites (bytes UTF-8, sin contar el terminador NUL): `source` 32,
`title` 64 y `artist` 48. Las cadenas vacias estan permitidas para
representar ausencia de metadatos. `session_id` debe tener 36 caracteres;
ademas debe coincidir con la sesion activa. Las claves desconocidas,
duplicadas, los campos omitidos, los tipos distintos de string, caracteres
de control y desbordamientos se rechazan. El reloj de lease de 30 s se
mantiene sin alteracion; esta llamada NO prolonga la sesion.

**Respuesta prevista** en caso de exito:

```json
{"accepted":true,"session_id":"550e8400-e29b-41d4-a716-446655440000","revision":1}
```

`revision` debe aumentar dentro de cada sesión. Repetidos o atrasados generan HTTP 409. El Content-Type debe ser exactamente `application/json` (415 en caso contrario). Una nueva sesión comienza con revision 1. Esta versión rompe la compatibilidad del borrador de cuatro campos y sigue desactivada en capacidades públicas.

**Errores previstos:** `401` por bearer/token de sesion incorrecto, `403`
por permiso/propietario, `404` por sesion ausente o ID obsoleto,
`400` por JSON invalido y `500` si el display no esta disponible.
El error mantiene el envelope canonico (`error.code`, `message`,
`request_id`, `details`). Nunca se escriben tokens ni cadenas arbitrarias
en logs.

## Funcionamiento de Stream

El API valida el JSON y copia todos los datos a buffers locales antes de
liberar la estructura cJSON. La capa `michi_session` comprueba token,
propietario, id de sesion, expiracion del lease y vida del engine bajo su
mutex. El renderizador copia buffers acotados y solicita un redraw no
bloqueante. La informacion se borra al cerrar una sesion, asi que no se
arrastra el titulo de una sesion anterior.

La fuente o sala se muestra arriba a la derecha en PLAYING, PAUSED y
BUFFERING; cuando no hay informacion de fuente, no se pinta una etiqueta
ficticia. El volumen y formato de audio permanecen en el footer.

## Trabajo requerido en Micro Server (NO realizado en esta carpeta)

1. Obtener el `session_id` y el token de sesion correctos tras POST session.
2. Enviar PUT con los 4 campos cuando comienza o cambia una pista, con
   Bearer Michi Home y token de sesion. No llamar con credenciales antiguas.
3. No enviar si `features.now_playing=false` salvo en una prueba manual
   autorizada o con feature flag de desarrollo explicitamente habilitado.
4. Implementar fixtures de contrato comunes y pruebas de metadata real,
   session close, metadata stale, expired token y UTF-8.
5. Subir `features.now_playing=true` SOLO tras demostrar la ruta completa.

## Validacion pendiente

Esta entrega **no ejecuto tests ni compilo ESP-IDF**. OpenCode debe probar el
parser, validacion de identidad, autorizacion cruzada, metadata sin sesion,
actualizacion de titulo, pausa y parada; inspeccionar UI en LCD y ver el
caso real Micro Server→Stream. No publicar soporte sin esas evidencias.

**ADVERTENCIA:** no se ha actualizado Michi Link alpha5 ni el productor Micro Server. No anunciar `features.now_playing=true` sin homologación.

## Sprint H3 — Paridad del simulador y telemetría sin datos privados

La implementación de simulador `simulator/receiver_sim.py` dispone ahora del
mismo endpoint **local experimental** `PUT /api/v1/receiver-lite/now-playing`.
Requiere Bearer Michi Home válido, `X-Michi-Session`, tipo de contenido JSON exacto,
identidad de propietario de la sesión, `session_id` activo y `revision` uint32
estrictamente creciente. Rechaza campos adicionales, claves duplicadas,
cadenas con controles, UTF-8 inválido, límites de tamaño y revisiones obsoletas.
Se comprueban fin de sesión y expiración de lease antes de aplicar metadatos.
La simulación conserva los metadatos exclusivamente en RAM y los elimina al
cerrar o expirar la sesión; el simulador no tiene pantalla LCD física.

**Esto no modifica los esquemas vendorizados alpha.5** ni convierte la
capacidad opcional en oficial: `features.now_playing=false` continúa sin cambio.
La suite de regresión de `simulator/tests/test_integration_http.py` incluye
casos de autorización cruzada, revisión duplicada, JSON adversarial, caducidad,
actualización y limpieza. Los tests se ejecutarán en el equipo local del usuario.

Adicionalmente, el firmware añade el campo `session.metadata_revision` en la
respuesta protegida de `/api/v1/receiver-lite/diagnostics`. Para una sesión
activa, 0 significa que aún no se ha aceptado ningún PUT y N refleja la última
revisión aceptada; sin sesión, el objeto `session` informa `active:false` y no
inventa una revisión. Esto permite a la WebUI correlacionar la revisión que
Micro Server intentó enviar con la aceptada por Stream sin exponer títulos,
artistas, claves, tokens ni identidad del usuario en diagnósticos. No confundir
`metadata_revision > 0` con recepción de audio RTP válida.

**Contrato pendiente para Micro Server:** añadir un productor de metadatos
ordenados que incremente `revision` por cada cambio en una sesión, reinicie el
contador al crear otra sesión, utilice el Bearer y token RTP vigentes, trate
409 como desactualización y compare con `session.metadata_revision` en
la WebUI. Por el feature flag `false`, debe habilitarse solo en prueba
controlada hasta homologar la extensión y publicar contratos compartidos.

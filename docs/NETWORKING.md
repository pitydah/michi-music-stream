# Michi Music Stream — Red y descubrimiento actuales

## Wi-Fi

Wi-Fi Station con configuración importada desde el paquete de fábrica. Sin punto de acceso ni dependencia del botón de pairing. No imprimir SSID/contraseñas sensibles en logs de producción.

## Descubrimiento

- mDNS: servicio `_michi-link._tcp.local` en el puerto HTTP configurado, con identidad de receptor y rol `audio_receiver`.
- Anuncio firmado: UDP multicast `224.0.0.167:53318`, TTL 1, datagrama único y esquema criptográfico de Michi Link.
- Seguridad horaria: los anuncios firmados dependen de un reloj válido. **Si SNTP no sincroniza, diagnóstico explícito**: diferenciar ausencia de mDNS, espera de reloj y rechazo de firma. No deshabilitar verificación de firmas para desbloquear el descubrimiento.
- Diagnóstico inter-repositorio: Micro Server/WebUI debe mostrar IP, mDNS visto, datagrama recibido, verificación de firma, `michi_id`, `home_id`, estado de confianza y último error.

## Autenticación

Michi Home `HOME_MEMBERSHIP`: desafío en `/auth/challenge`, prueba firmada y certificado de pertenencia en `/auth/session`; token Bearer temporal para operaciones de sesión. Revocaciones firmadas por autoridad Home. No hay pairing físico por PIN en las rutas operativas.

## RTP

UDP con puerto negociado, PT=97 y PCM S16LE 48 kHz estéreo, 10 ms/paquete (1920 bytes). Validar IP de origen, SSRC, versión RTP y tamaño; no usar 'first packet wins'. Lease de 30 s renovado por heartbeat; sin RTCP/FEC ni cifrado RTP.

## Límites

Solo LAN. TLS/seguridad de arranque, cifrado de flash y pruebas de recuperación hardware aún necesitan evidencia de certificación.

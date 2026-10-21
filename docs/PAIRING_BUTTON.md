# Emparejamiento por botón — documentación histórica (retirada)

> **NO es el procedimiento de instalación actual.** Esta guía describe el protocolo RECEIVER_BUTTON antiguo. El firmware vigente registra autenticación `HOME_MEMBERSHIP`, no los endpoints `/api/v1/pair/*`; ya no compila un componente `michi_button` ni `michi_pairing`.

## Procedimiento vigente

1. La herramienta Michi Ecosystem genera la identidad y la membresía emitida por la autoridad Michi Home.
2. La configuración de fábrica incluye la red Wi-Fi y la identidad/membresía; no registrar contraseñas en logs ni en el repositorio.
3. El Stream importa el paquete de fábrica, se conecta como estación Wi-Fi, publica mDNS y anuncia multicast firmado cuando dispone de hora válida.
4. Micro Server consulta `/api/v1/server/info`, inicia `POST /api/v1/auth/challenge` y completa `POST /api/v1/auth/session` con prueba criptográfica de su membresía al mismo Home.
5. La sesión de audio se abre por `/api/v1/receiver-lite/session`.

## Recuperación pendiente

La supresión del botón obliga a definir y probar un proceso alternativo de recuperación ante Wi-Fi inválida, autoridad de Home corrupta o flasheo interrumpido. **No considerar cubierto este requisito** por la documentación histórica. La recuperación debe realizarse mediante USB/serial o un flujo seguro de reaprovisionamiento verificable, sin dar acceso irrestricto a controladores de otra identidad.

## Compatibilidad

Los archivos `examples/pair-*.json` y los flags de pairing del simulador son testigos históricos y NO describen endpoints activos del firmware. No reintroducir las rutas físicas para hacer pasar pruebas antiguas.

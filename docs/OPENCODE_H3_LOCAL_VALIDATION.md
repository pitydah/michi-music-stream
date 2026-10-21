# OpenCode — Validación local del Sprint H3 (NO modificar contract alpha5)

Trabaja solo sobre mi repositorio LOCAL de michi-music-stream después de
reemplazar los fuentes descargados de Drive. Verifica `git status --short`,
`git diff --stat` y el contenido del contrato `VERSION`/`UPSTREAM_COMMIT`.
No supongas que un binario o resultado E2E histórico certifica esta versión.

1. Examina el diff de H3 y confirma que `features.now_playing=false` y que no
   hay cambios de contrato RTP, Wi-Fi, pairing o OTA inesperados.
2. Ejecuta los tests de simulador HTTP y escenarios, con énfasis en
   `TestNowPlayingExperimental`: orden creciente, duplicados, propietario,
   Bearer inválido, token RTP erróneo, lease caducado, UTF-8, NUL, JSON
   malformado, reset de contador al cerrar y abrir nueva sesión.
3. Ejecuta `make -C tests/host clean test` y verifica que
   `michi_session_get_info().metadata_revision` refleja 0, luego 1 y 2,
   mientras una escritura rechazada no modifica el valor. Si hace falta,
   añade prueba de contrato de `GET /diagnostics` para snapshot y ausencia
   de títulos, tokens o credenciales.
4. Ejecuta `./scripts/run_tests.sh --full`; registra el primer fallo con
   traza completa. No suprimas ni skips suites para aparentar PASS.
5. Compila limpio ESP-IDF 5.3 para ESP32-S3 (desde entorno local configurado).
   Comprueba los perfiles Standard/Hi-Fi sin usar artefactos anteriores.
6. Revisa paridad del nuevo endpoint con el firmware: códigos HTTP, límites de
   bytes UTF-8 y tipo JSON, dueño, lease y generación de sesión. La
   simulación es un TEST DOUBLE, no garantía de exactitud física.
7. Solo cuando lo anterior esté verde, prepara integración controlada en
   Michi Micro Server (sin activar feature oficialmente): productor con
   revisiones ordenadas, WebUI con revisiones enviadas y aceptadas, errores
   visibles, y prueba real Stream→LCD→I2S con PCM5122.
8. Entrega informe con revisión Git, cantidad de casos PASS/FAIL, uso de
   memoria, cualquier error de compilación, riesgos remanentes y
   recomendaciones puntuales de corrección. No declares la certificación física de extremo a extremo
   sin evidencia del dispositivo y de la cadena completa.

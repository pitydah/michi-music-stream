# Michi Music Stream — actualización de código sobre Google Drive

Fecha: 18 de octubre de 2026. Carpeta: `michi-music-stream`.

## Cambios escritos en Drive

- `firmware/components/michi_http/http_server.c`: especificación del API acorde con la tabla de rutas real (Michi Home, no pairing físico).
- `simulator/receiver_sim.py`: revocaciones verificadas contra la firma Ed25519 de la autoridad Home, el hogar de pertenencia y el schema vendorizado antes de cambiar estado. Los antiguos parámetros CLI de pairing permanecen como fixtures históricos.
- `tests/e2e/test_e2e_micro_stream.py`: regresión para revocación falsificada, revocación firmada válida y Home diferente.
- `tests/e2e/run_e2e.py`: provenance real desde `VERSION` y `UPSTREAM_COMMIT`, HEAD de Git y estado dirty declarado; informe por versión real del bundle. La etiqueta alpha6 anterior no prueba un bundle alpha6 si el contenido instalado es alpha5.
- `.github/workflows/ci.yml` y `scripts/run_tests.sh`: detección explícita de la fuente host ausente y limpieza de rutas de include obsoletas.
- `docs/OVERVIEW.md`, `NETWORKING.md`, `ROADMAP.md`, `PAIRING_BUTTON.md`, `HARDWARE_STANDARD.md`: descripción del producto actual y límites pendientes.
- `README.md`, `simulator/README.md`, `firmware/README.md`: eliminación de instrucciones activas para pairing físico o avisos claros cuando una sección es histórica.

## Bloqueos no resueltos por esta edición

1. El fuente `tests/host/test_michi_home.c` está ausente en la carpeta de Drive inspeccionada: **restaurar el archivo verdadero desde la copia local/Git**; no crear una implementación falsa ni confiar en binarios host precompilados.
2. No se actualizó el bundle `contracts/michi-link` a alpha6 porque la fuente no está verificada como equivalente; conserva el pin alpha5 exacto.
3. No se ejecutó compilación ni pytest en Drive; realizar validaciones después de descargar a CachyOS/Arch.
4. Queda pendiente la prueba Micro Server→Stream sobre ESP32-S3 y DAC y la visibilidad completa en WebUI del servidor.
5. Los directorios de build, `.pytest_cache`, binarios y `.git` se dejaron intactos para no alterar la copia original sin autorización específica de limpieza.

## Prompt para OpenCode / Codex — validación LOCAL

> Trabaja sobre el repositorio LOCAL después de reemplazar los archivos de Drive. Antes de ejecutar pruebas, muestra `git status --short`, `git diff --stat` y el contenido real de `contracts/michi-link/VERSION` y `UPSTREAM_COMMIT`. No instales ni sustituyas unilateralmente un bundle distinto.
>
> 1. Comprueba que `tests/host/test_michi_home.c` está presente. Si falta, detente e indica origen exacto que se debe recuperar. No ocultes la ausencia ni omitas tests host.
> 2. Valida sintaxis Python y lint de los archivos cambiados; revisa que el endpoint `/home/revocations` del simulador valide la firma de la autoridad Home de forma congruente con `michi_home.c`.
> 3. Ejecuta el runner completo `./scripts/run_tests.sh --full`; incluye los tests E2E de revocación falsa y válida. Comunica el primer error y su traza completa, sin afirmar PASS si falla.
> 4. Compila el firmware ESP-IDF 5.3 para ESP32-S3 y verifica CI de ambos perfiles de DAC sin recurrir a binarios anteriores.
> 5. Ejecuta la prueba en red LAN real: mDNS, UDP 224.0.0.167:53318, sincronización de reloj, autenticación por Michi Home, sesión RTP, heartbeat, audio PCM por PCM5122 y recuperación. Distingue `MOCK_PASS` de hardware probado.
> 6. Entrega un informe con HEAD/commit, archivos tocados, suite y cantidad de tests PASS/FAIL, errores, matriz de hardware y plan de corrección. No cambies la decisión de eliminar el botón de pairing.

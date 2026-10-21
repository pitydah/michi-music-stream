# OpenCode / Antigravity — validación posterior H16

H16 fue editado en Drive sin ejecutar compilación o tests. Cargar H16 sobre el snapshot original actual, sin reemplazar `.git` local.

1. `git diff --check` y revisión del manifiesto H16. Asegurar que `test_michi_session.c` real se enlaza con `shim/freertos/queue.c` y `shim/esp_timer.c`.
2. `make -C tests/host clean && make -C tests/host test`; `./scripts/run_tests.sh --full`; `idf.py build` dentro de `firmware`.
3. Comprobar test H16 con inyección de cola rechazada y rearme fallido, sin consultas HTTP antes del evento; revisar race con timer fake, cola y task pthread. No utilizar binarios preexistentes.
4. Garantizar 1s periodic timeout real (10ms solo en MICHI_HOST_TEST), que no se permite bloquear `esp_timer` callback y que no hay UAF/locks extendidos.
5. Reportar FAIL, logs, HEAD y diff, corregir las fuentes y repetir toda la suite. No afirmar PASS si no hay ejecución.

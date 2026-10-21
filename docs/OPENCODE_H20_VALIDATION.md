# OpenCode / Antigravity — H20 (al reemplazar la carpeta local)

No dar por aprobadas pruebas aún no ejecutadas. Trabajar contra el árbol fuente reconciliado y conservar el Git local.

1. `python scripts/reconcile_drive_sources.py --root .` debe devolver estado `complete=true` (después de una reconciliación segura y copia de seguridad externa).
2. `python scripts/check_release_profile.py --root . --profile lab`; después modo `production`, usando el overlay separado, sin imponerlo a los dispositivos existentes.
3. `python -m pytest tests/test_reconcile_drive_sources.py tests/test_release_profile.py tests/test_source_snapshot.py -q` y solucionar cualquier defecto del código de pruebas.
4. `make -C tests/host clean && make -C tests/host test` sin saltarse el motor RTP real, diagnósticos ni Wi-Fi security.
5. `./scripts/run_tests.sh --full`, inspeccionar el resultado verdadero, luego `idf.py build` con ESP-IDF objetivo y configuración release seleccionada.
6. Comprobar que la estructura real de `GET /diagnostics` es compatible con el productor y que la API no declara salida analógica probada.
7. Preparar pruebas físicas ESP32-S3/PCM5122 y Micro Server en una fase separada.

Si cualquier prueba falla, registrar salida, archivo, línea y corrección; no sustituir el bug por un mock ni rebajar checks para conseguir PASS.

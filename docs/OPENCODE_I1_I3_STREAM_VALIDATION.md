# OpenCode — validación local de Michi Music Stream (I1/I3)

Usa los archivos descargados de Google Drive como fuente editada y el repositorio local como autoridad Git (NO reemplaces `.git`). No recompiles a partir de binarios `build/` antiguos ni otorgues PASS por artefactos de Drive.

**Meta:** compilar y certificar importación `MICHI-F1`, anuncios Ed25519 y el generador de candidato factory; detectar/regresar fallos sin alterar audio estable ni autoridades de Home. Cargar skills de C/ESP-IDF, seguridad, pytest y protocolos disponibles en OpenCode antes de la tarea.

1. Verifica `git status`, diff completo y `git diff --check`; informa archivos y SHAs.
2. Corre suites C host de fábrica/descubrimiento y todas las restantes mediante `./scripts/run_tests.sh --full`. Si alguna prueba falla, corrige la fuente exacta y añade regresión correspondiente. **No ocultes skip ni marques verde sin evidencias.**
3. Compila ESP-IDF desde configuración limpia, revisa uso de RAM/heap y no alteres flash/particiones accidentalmente.
4. Prueba parsing estricto `MICHI-F1` y la firma canónica para nombres con comillas, barra invertida, Unicode español y metadatos Home opcionales; compara con vectores oficiales Michi Link alpha.5.
5. Crea un build candidato usando `build_factory_manifest.py` con datos reales del board; comprueba todos sus segmentos con el verificador de Michi Ecosystem. Prueba errores intencionales en fixtures sintéticos. **No publiques ni flashees sin aprobación del usuario.**
6. Revisa que `factory_partition` y `update_segments` jamás autoricen sobrescribir NVS/identidad en UPDATE; que el canal de desarrollo no sea confundido con release firmado.
7. Entrega informe: PASS/FAIL/SKIP por comando, diffs adicionales, riesgos abiertos, criterios P0 pendientes y acciones separadas para Ecosystem/Michi Link/Micro Server. Genera commit tras verificación local, no edites la copia Drive automáticamente.

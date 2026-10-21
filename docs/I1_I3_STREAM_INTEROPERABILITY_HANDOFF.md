# Michi Music Stream — I1/I3, correcciones de interoperabilidad y artefactos Factory

**Estado:** CAMBIOS DE FUENTE, NO CERTIFICADOS. Editados sobre la copia de Google Drive. La compilación, suites de host, contrato alpha.5, flasheo y hardware se ejecutan posteriormente con OpenCode.

## Cambios entregados

1. `firmware/components/michi_home/factory_cfg.c`
   - Un contenedor binario `MICHI-F1` truncado, CRC incorrecto o con versión desconocida ya no cae en un parser permisivo de líneas/JSON. Se conserva la lectura de registros heredados válidos y de la forma estándar con cabecera de 27 bytes y CRC32 big-endian.
   - JSON estructurado se exige íntegro; bytes NUL, raíces no objeto y material adicional se rechazan.
   - NO habilita reprovisión automática de Home ni transforma CRC32 en firma. Sigue requiriéndose autorización OWNER/USB para operaciones destructivas.
2. `firmware/components/michi_discovery/announce.c`
   - La canonicalización firmada usa strings escapados por cJSON; desaparece la interpolación manual insegura de `name` u otros campos.
   - Los campos opcionales Home vacíos se excluyen tanto de los bytes firmados como del JSON emitido.
   - NO altera las etiquetas criptográficas, el orden lexicográfico de campos, Ed25519 ni los contratos del bundle.
3. `scripts/build_factory_manifest.py`
   - Genera **manifiestos de fábrica exclusivamente para desarrollo** desde `build/flasher_args.json`, `partitions.csv` y binarios ESP-IDF existentes.
   - Descubre offsets de los artefactos desde ESP-IDF y de `factory_cfg` desde el layout. Rechaza segmentos ambiguos, fuera de flash, con traslapes o mal dimensionados.
   - Copia imágenes a una carpeta nueva y publica SHA-256/size/offset, `factory_partition`, `update_segments=["app"]`, `partition_table_digest`, flash size y metadatos explícitos.
   - **No firma bundles**, **no crea releases**, **no escribe la placa**, **no incluye material privado ni blob Wi-Fi**. Un manifiesto `channel=dev` no debe ser aceptado como producción por Ecosystem.
4. `tests/host/test_michi_home.c` y `tests/host/test_michi_discovery.c`: nuevos casos de regresión no ejecutados (esquema desconocido, truncado, JSON extra, nombres con JSON escaping).

## Comando ilustrativo de trabajo local (sustituir argumentos con evidencia real)

```bash
python3 scripts/build_factory_manifest.py \
  --build firmware/build \
  --partitions firmware/partitions.csv \
  --out /tmp/michi-stream-factory-candidate-UNIQUE \
  --flash-size 0x1000000 \
  --board <BOARD_ID_VERIFICADO> \
  --source-commit "$(git rev-parse HEAD)" \
  --firmware-version <VERSION_REAL> \
  --minimum-ecosystem-version <VERSION_CONFIRMADA>
```

El generador no debe ejecutarse si no existe build ESP-IDF adecuado o si no se verificó la capacidad física de flash. La revisión del manifiesto desde Ecosystem debe aplicar los controles reales de `manifest_verifier.py` y aprobación de operaciones. Nunca confundir `channel=dev` con un release oficial firmado.

## Contratos que NO se modificaron

- Ecosystem está vendorizado en Michi Link **alpha.2**, Stream en **alpha.5**. La sincronización y las nuevas pruebas de golden vectors pertenecen a Ecosystem/Michi Link y requieren un PR certificado, no parchear manifests manualmente.
- El bootstrap `MICHI-F1` v1 solamente transporta red/nombre en Ecosystem. La ampliación Home/identidad y entrega física de la clave privada NO están congeladas. El importador de Stream admite campos Home, pero eso no demuestra una transacción segura de fábrica.
- No se alteraron RTP, I2S, DAC, BLE, OTA ni los permisos del roster. Nada en esta entrega constituye una certificación de audio físico o incorporación E2E.

## Puertas para OpenCode (pendientes, sin PASS declarados)

1. `git diff --check`, compilación completa ESP-IDF limpia y perfiles Standard/Hi-Fi.
2. Suite host completa (`./scripts/run_tests.sh --full`) incluyendo golden vector alpha.5, factory/identity y discover. Verificar que no se rompió canonicalización existente.
3. Pruebas negativas de `MICHI-F1`: CRC erróneo, header corto, tamaño desbordado, esquema futuro, JSON extra, NUL incrustado, llave privada discordante, membresía ajena, Home ya instalado y retry tras corte de energía.
4. Pruebas de manifiesto en carpeta sintética y build real: hash alterado, archivo ausente, flash insuficiente, offsets duplicados, `factory_cfg` fuera del chip, imagen grande y política de UPDATE sin permitir NVS/Factory.
5. Compatibilidad con `michi_ecosystem.infrastructure.flashing.manifest_verifier` sin falsificar soporte de firma ni release. Capturar artefactos y commit exacto.
6. LCD, Wi-Fi, anuncio firmado alpha.5, Home roster, Micro Server/WEBUI y PCM5122 físico sólo cuando el hardware esté disponible.
7. Corregir cualquier error resultante antes de promocionar a release o tocar los locks upstream.

## Nota de seguridad

CRC32 sólo detecta corrupción accidental. La autoridad Home no debe confiar en blobs escritos por USB sin control de acceso. Propuesta preferente de producción: clave Ed25519 generada y almacenada en el receptor + confirmación de propiedad/identidad mediante un protocolo USB/attestation documentado. Requiere diseño contractual y prueba con Ecosystem, no está implementada aquí.

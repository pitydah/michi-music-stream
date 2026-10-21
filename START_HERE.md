# Michi Music Stream — Google Drive, H22 (fuentes PRETEST)

**La carpeta `michi-music-stream/` ya contiene los archivos fuente H22 editados en sus rutas originales.** Puedes descargar **esta carpeta completa** desde Drive. No necesitas superponer ningún ZIP histórico ni copiar parches manualmente.

**IMPORTANTE:** La carpeta original de Drive también contiene `.git`, `firmware/build`, `firmware/managed_components`, cachés, ejecutables host y ZIP históricos. **Nunca reemplaces la carpeta Git local arrastrando todo el contenido de Drive encima de ella.** Conserva el `.git` local, las credenciales y tus datos.

La forma segura de reemplazar **únicamente las fuentes H22** está en `SOURCE_BASELINE_H22.md`: usa `scripts/install_drive_sources.py`, que comprueba el inventario `docs/H22_SOURCE_TREE_MANIFEST.json`, hace vista previa por defecto y respalda los archivos sustituidos antes de copiar. No ejecuta tests ni flashea el ESP32-S3.

`docs/MICHI_MUSIC_STREAM_H22_COMPLETE_SOURCE_PRETEST.zip` es una alternativa de archivo limpio para inspección; ya no es un requisito para usar la carpeta descargada.

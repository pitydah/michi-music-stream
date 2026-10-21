# MICHI MUSIC STREAM — H5 KILLCRITIC (revisión 2026-10-19)

## Estado real

**Durante esta pasada se integraron directamente en Google Drive los archivos de H4 y H5** (motor de audio, jitter buffer, cabecera, Kconfig y pruebas host), y se añadió la cabecera portable de política de errores de recepción. Existe también un ZIP completo como respaldo. **Las pruebas NO se han ejecutado ni compilado.** Las tareas de Ecosystem/Michi Link quedan fuera de este sprint.

## Hallazgos confirmados en código

1. RTP: el tratamiento de `recvfrom(n < 0)` ocultaba fallos permanentes de socket del mismo modo que un timeout `SO_RCVTIMEO`. H5 distingue `EAGAIN/EWOULDBLOCK/EINTR` de fallos persistentes y termina la sesión tras **tres errores no transitorios consecutivos**. El valor está en `michi_audio_recv_policy.h`, con tests host deterministas. Para fallos persistentes se reporta un error de estado y se libera la sesión; **no se anuncia reconexión automática**.
2. Recuperación de underrun: la selección posterior a `session_fill()` podía usar `michi_jb_oldest()` sobre un conjunto que no había purgado entradas antiguas, y retroceder el playhead. H5 descarta explícitamente obsoletos en la recuperación, igual que H4 en el playout normal.
3. Un fallo de `write_silence()` terminaba el bucle por error de salida, pero no garantizaba limpiar el flag `s_session_run`. H5 asegura que las salidas autónomas lo desactiven; reduce estados inconsistentes, aún requieren pruebas de concurrencia.
4. Datagrama UDP de 0 bytes: antes se ignoraba indistinguiblemente de un timeout. H5 lo cuenta como malformado sin contaminar PCM.
5. H4 aún pendiente: 50 slots x 8192 bytes en PSRAM, frente a 50 x 1920 bytes PCM; 306,25 KiB recuperables; clipping de longitud de payload y rechazo de `SO_RCVTIMEO` fallido. **Integrado en Drive, pero no certificado hasta ejecutar pruebas locales y físicas.**

## Avance porcentual estimado

**Michi Music Stream: 67 % global / 33 % restante** hacia una v1 funcional, segura y certificada. Es una estimación ponderada y con incertidumbre; no una medición de tests. El progreso *implementado y certificado* es menor. **Los cambios H4/H5 están escritos en Drive, pero el porcentaje de funcionamiento certificado NO aumenta hasta compilar, correr tests y probar el hardware.**

| Área | Madurez estimada | Bloqueo principal |
|---|---:|---|
| Arquitectura de firmware | 87% | robustez estados y fallos |
| RTP/jitter/audio lógico | 77% | integración H4+H5, estrés real, MTU |
| I2S/DAC | 74% | rendimiento real PCM5122, underruns, ruido |
| Wi-Fi/identidad/Home | 68% | flasheo Home-aware y recuperación USB |
| Descubrimiento Micro Server | 50% | reconocimiento real + WebUI |
| Sesiones, control y HTTP | 77% | CI/E2E y error recovery |
| UI y metadatos | 77% | productor Micro Server, LCD real |
| OTA y seguridad | 53% | OTA recuperable, clave, canal seguro |
| CI, pruebas y trazabilidad | 65% | resultados recientes y liberación hardware |
| E2E y certificación física | 20% | cadena Player→Micro Server→Stream→PCM5122 |

## Prioridades estrictas después de H5

- **P0:** Descargar la carpeta con H4+H5 ya integrado y revisar Git diff limpio con el baseline local; ejecutar host tests, ESP-IDF, CI y comprobar límites en ambas variantes de hardware.
- **P0:** E2E real con Micro Server y su WebUI (descubrimiento, auth, sesión, metadata revision, UDP a DAC). No fingir conectado por anuncio enviado.
- **P0:** Aprovisionamiento Home con Ecosystem sin botón y recuperación USB segura; bloquear altas falsas.
- **P0:** OTA con rollback efectivo, datos NVS persistentes y protección de secretos; pruebas con fallo/corte.
- **P1:** Medir latencia, memoria libre mínima, fragmentación IP, jitter real y pérdidas de 1-10%; comparar H3 vs H5.
- **P1:** Estados de error de red, Wi-Fi y DAC reflejados en LCD/WebUI de Micro Server.
- **P2:** pulido visual final y matrices de perfiles Standard/Hi-Fi.

**No se modifican RTP wire contract, Michi Link, MICHI-F1/Home, OTA o el motor I2S en H5.**

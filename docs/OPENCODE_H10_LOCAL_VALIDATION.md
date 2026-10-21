# Prompt para OpenCode — Validación de Michi Music Stream H10

Actúa como ingeniero senior ESP-IDF y auditor KILLCRITIC. Tu misión es **validar y, si aparecen defectos, corregir** el sprint H10 aplicado a la copia local de Michi Music Stream, preservando las mejoras H1–H9. No reemplaces el firmware entero, no alteres Michi Link, Ecosystem ni Micro Server, no borres secretos o identidades NVS.

## Contexto y foco

`firmware/components/michi_audio_output/michi_audio_output.c` ya tiene un consumidor I2S cooperativo. H10 corrige tres fallos de ownership: `disable` fallido durante STOP, `del_channel` fallido en DEINIT y rollback fallido de `xTaskCreate`. Los fallos deben ser explícitos, recuperables y no liberar recursos retenidos por el driver. El shim `tests/host/shim/i2s_shim.c` permite inyectar errores one-shot.

## Procedimiento

1. Abre el repositorio local, documenta `git status`, HEAD y diffs H10. Conserva el trabajo local ajeno a H10.
2. Ejecuta las pruebas host apropiadas, incluyendo `make -C tests/host test` (si hay errores de build por herramientas faltantes, indícalos); compila con advertencias estrictas.
3. Verifica las regresiones H10: tras fallo `i2s_channel_disable`, `stop()` retorna error y estado FAULTED; se rechazan `start` y `deinit`; un segundo STOP puede recuperar. Tras fallo `i2s_del_channel`, `deinit()` retorna error, retiene recursos, bloquea START y permite un nuevo DEINIT. En fallo encadenado de `xTaskCreate` y rollback disable, no se habilita un segundo START; STOP recupera.
4. Ejecuta compilación ESP-IDF real (`idf.py build` desde `firmware/` con entorno disponible) y comprueba compatibilidad C11/FreeRTOS, macros Kconfig e interfaces I2S ESP-IDF específicas del proyecto.
5. Audita las intercalaciones STOP vs quiesce/resume, STOP timeout vs retry, fallo DMA vs STOP, init -> deinit, tareas pendientes y `vTaskDelete(NULL)`; investiga posibles deadlocks, dobles frees, UAF, semáforos eliminados bajo uso y corrupción del estado. No declares seguridad del concurrente sin evidencia.
6. Agrega o ajusta casos fallidos en la suite si los mocks permiten falsos positivos. No modifiques la geometría RTP ni el bootstrap de Ecosystem/Michi Link.
7. Si tienes ESP32-S3 con DAC compatible, ejecuta además pruebas controladas de sesiones STOP/START, desconexión y 24h sin garantizar resultados que no hayas medido. Un mock no certifica hardware.
8. Emite un informe con versión, SHA final, comandos y resultados, cobertura real y pendientes P0/P1/P2. Marca claramente PASS/FAIL/BLOCKED por prueba. Solo después de PASS completo crea un commit Git.

**Regla:** no inventar porcentajes ni marcar el producto como certificado por el mero éxito de host tests. No ejecutar scripts de flasheo destructivo sin autorización explícita del propietario.

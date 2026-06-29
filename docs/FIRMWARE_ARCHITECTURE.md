# Arquitectura del Firmware

## Plataforma

| Aspecto | Tecnología |
|---------|-----------|
| Framework | ESP-IDF v5.x |
| Lenguaje | C (C11) |
| RTOS | FreeRTOS |
| Toolchain | xtensa-esp32-elf |

## Módulos

```
firmware/common/
├── michi_link_lite      Servidor HTTP REST + handlers de endpoints
├── discovery            Anuncio mDNS _michi-receiver._tcp
├── pairing              Ventana 120 s, NVS, validación token
├── receiver_info        JSON dinámico vía config.h
├── heartbeat            Timeout 90 s con callback
├── session              Ciclo de vida de sesión de audio
├── volume               Ganancia digital 0-100
└── audio_output         Buffer circular + UDP rx + I2S tx
```

## Tareas FreeRTOS

| Tarea | Prioridad | Stack | Descripción |
|-------|-----------|-------|-------------|
| HTTP Server | 5 | 4096 | esp_http_server |
| Discovery | 3 | 2048 | mDNS announce |
| LED | 2 | 1024 | WS2812B |
| Audio Pipeline | 6 | 4096 | UDP + buffer + I2S |
| Session Manager | 4 | 2048 | Heartbeat + lifecycle |

## Secuencia de inicialización

1. `nvs_flash_init()`
2. Wi-Fi Station
3. GPIO botón pairing (interrupción)
4. `pairing_init()` (cargar NVS)
5. `discovery_init()` + `discovery_announce()`
6. `michi_link_lite_init()` (servidor HTTP)
7. `michi_link_lite_register_endpoints()`
8. `session_init()`
9. `heartbeat_init()`
10. Loop: check pairing window + heartbeat

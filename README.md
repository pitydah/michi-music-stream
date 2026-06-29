# Michi Music Stream

Familia de receptores físicos de audio para el ecosistema Michi.

**No es** biblioteca musical, ni administra playlists, ni indexa música, ni funciona como servidor.

**Es** una salida física de audio que recibe flujo desde Michi Music Player o Michi Micro Server y lo reproduce por jack 3.5 mm o RCA.

## Arquitectura

```
Mobile controla.
Player o Micro Server transmite.
Michi Stream reproduce.
```

## Productos

| Variante | Salida | DAC | Audio | Uso |
|----------|--------|-----|-------|-----|
| Standard | Jack 3.5 mm | I2S básico | PCM 16-bit / 48 kHz | Cocina, dormitorio, auxiliar |
| Hi-Fi | RCA estéreo | DAC I2S Hi-Fi | PCM 24-bit / 96 kHz | Living, amplificador, Hi-Fi |

## Protocolo: Michi Link v1-lite

REST HTTP en puerto 80 + audio por UDP. Discovery via mDNS.

| Método | Endpoint | Auth | Descripción |
|--------|----------|------|-------------|
| GET | `/api/v1/receiver/info` | No | Información del dispositivo |
| GET | `/api/v1/receiver/firmware` | No | Versión de firmware |
| POST | `/api/v1/receiver/pair/start` | No | Inicia ventana de pairing |
| POST | `/api/v1/receiver/pair/confirm` | No | Confirma emparejamiento |
| POST | `/api/v1/receiver/heartbeat` | Bearer | Latido de sesión |
| POST | `/api/v1/receiver/session/start` | Bearer | Inicia sesión de audio |
| POST | `/api/v1/receiver/session/stop` | Bearer | Detiene sesión |
| POST | `/api/v1/receiver/volume` | Bearer | Ajusta volumen (0-100) |

## Flujo

1. Receptor enciende → Wi-Fi → mDNS `_michi-receiver._tcp`
2. Player/Micro Server descubre receptor
3. Usuario presiona botón físico de pairing (ventana 120 s)
4. Controlador envía pair/start → pair/confirm → token guardado en NVS
5. Controlador envía session/start → receptor abre puerto UDP
6. Controlador envía audio PCM/Opus por UDP → receptor reproduce por I2S DAC
7. Heartbeat cada 30 s; sin heartbeat por 90 s → sesión se detiene

## Estructura

```
michi-music-stream/
├── README.md
├── LICENSE
├── docs/             # Documentación técnica (9 archivos)
├── firmware/         # Código fuente ESP-IDF
│   ├── common/       # 8 módulos compartidos
│   ├── standard/     # Build Standard
│   └── hifi/         # Build Hi-Fi
├── examples/         # 5+ payloads JSON
└── tests/
    └── contract/     # Tests de validación de contrato
```

# Michi Music Stream Hi-Fi

ESP32-S3 + PCM5122 + RCA estéreo.

## Build

```bash
idf.py set-target esp32s3
idf.py menuconfig  # configurar Wi-Fi
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## GPIO

| Señal | Pin |
|-------|-----|
| MCLK | 9 |
| BCLK | 6 |
| LRC | 7 |
| DIN | 8 |
| SDA (PCM5122) | 1 |
| SCL (PCM5122) | 2 |
| Botón | 3 |
| LED | 4 |

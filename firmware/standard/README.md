# Michi Music Stream Standard

ESP32-S3 + PCM5102A + Jack 3.5 mm.

## Build

```bash
idf.py set-target esp32s3
idf.py menuconfig  # configurar Wi-Fi en "Michi Stream Configuration"
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## GPIO

| Señal | Pin |
|-------|-----|
| BCLK | 6 |
| LRC | 7 |
| DIN | 8 |
| Botón | 3 |
| LED | 4 |

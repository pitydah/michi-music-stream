#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_DEVICE_ID_STR  "rcv_standard_001"
#define DEVICE_NAME           "Michi Stream Cocina"
#define DEVICE_TYPE           "michi_stream_standard"
#define FIRMWARE_VERSION      "0.1.0"
#define BUILD_DATE            "2026-06-29"
#define API_VERSION           "v1-lite"

#define OUTPUT_CONNECTOR      "jack_3_5"
#define MAX_SAMPLE_RATE       48000
#define MAX_BIT_DEPTH         16

#define I2S_BCLK_PIN          GPIO_NUM_6
#define I2S_LRC_PIN           GPIO_NUM_7
#define I2S_DIN_PIN           GPIO_NUM_8
#define PAIRING_BUTTON_PIN    GPIO_NUM_3
#define LED_PIN               GPIO_NUM_4

#define SUPPORTED_CODECS      {"pcm_s16le", "opus"}
#define OTA_SUPPORTED         false

#endif

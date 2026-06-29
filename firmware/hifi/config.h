#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_DEVICE_ID_STR  "rcv_hifi_001"
#define DEVICE_NAME           "Michi Stream Living Hi-Fi"
#define DEVICE_TYPE           "michi_stream_hifi"
#define FIRMWARE_VERSION      "0.1.0"
#define BUILD_DATE            "2026-06-29"
#define API_VERSION           "v1-lite"

#define OUTPUT_CONNECTOR      "rca_stereo"
#define DAC_NAME              "hifi_i2s"
#define MAX_SAMPLE_RATE       96000
#define MAX_BIT_DEPTH         24

#define I2S_MCLK_PIN          GPIO_NUM_9
#define I2S_BCLK_PIN          GPIO_NUM_6
#define I2S_LRC_PIN           GPIO_NUM_7
#define I2S_DIN_PIN           GPIO_NUM_8
#define PAIRING_BUTTON_PIN    GPIO_NUM_3
#define LED_PIN               GPIO_NUM_4

#define SUPPORTED_CODECS      {"pcm_s16le", "pcm_s24le", "opus"}
#define OTA_SUPPORTED         true

#endif

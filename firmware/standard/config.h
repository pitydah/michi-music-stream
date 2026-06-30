#ifndef CONFIG_H
#define CONFIG_H

/* Si no se usa Kconfig, estos defaults aplican */
#ifndef CONFIG_MICHI_WIFI_SSID
#define CONFIG_MICHI_WIFI_SSID    "michi"
#endif
#ifndef CONFIG_MICHI_WIFI_PASSWORD
#define CONFIG_MICHI_WIFI_PASSWORD "michipass"
#endif

#ifndef CONFIG_MICHI_DEVICE_ID
#define CONFIG_DEVICE_ID_STR      "rcv_standard_001"
#else
#define CONFIG_DEVICE_ID_STR      CONFIG_MICHI_DEVICE_ID
#endif

#ifndef CONFIG_MICHI_DEVICE_NAME
#define DEVICE_NAME               "Michi Stream Cocina"
#else
#define DEVICE_NAME               CONFIG_MICHI_DEVICE_NAME
#endif

#define DEVICE_TYPE               "michi_stream_standard"
#define FIRMWARE_VERSION          "0.1.0"
#define BUILD_DATE                "2026-06-29"
#define API_VERSION               "v1-lite"

#define OUTPUT_CONNECTOR          "jack_3_5"
#define MAX_SAMPLE_RATE           48000
#define MAX_BIT_DEPTH             16

#define I2S_BCLK_PIN              GPIO_NUM_6
#define I2S_LRC_PIN               GPIO_NUM_7
#define I2S_DIN_PIN               GPIO_NUM_8
#define PAIRING_BUTTON_PIN        GPIO_NUM_3
#define LED_PIN                   GPIO_NUM_4

#define SUPPORTED_CODECS          {"pcm_s16le"}
#define OTA_SUPPORTED             false

#endif

#include <cJSON.h>
#include "receiver_info.h"
#include "config.h"

cJSON * receiver_info_get_json(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "device_id", CONFIG_DEVICE_ID_STR);
    cJSON_AddStringToObject(r, "name", DEVICE_NAME);
    cJSON_AddStringToObject(r, "type", DEVICE_TYPE);
    cJSON_AddStringToObject(r, "firmware", FIRMWARE_VERSION);
    cJSON_AddStringToObject(r, "api_version", API_VERSION);
    cJSON *ro = cJSON_AddArrayToObject(r, "roles");
    cJSON_AddItemToArray(ro, cJSON_CreateString("audio_receiver"));
    cJSON_AddItemToArray(ro, cJSON_CreateString("music_stream_receiver"));
    cJSON *o = cJSON_AddObjectToObject(r, "output");
    cJSON_AddStringToObject(o, "connector", OUTPUT_CONNECTOR);
    #ifdef DAC_NAME
    cJSON_AddStringToObject(o, "dac", DAC_NAME);
    #endif
    cJSON_AddNumberToObject(o, "max_sample_rate", MAX_SAMPLE_RATE);
    cJSON_AddNumberToObject(o, "max_bit_depth", MAX_BIT_DEPTH);
    cJSON_AddNumberToObject(o, "channels", 2);
    cJSON *cc = cJSON_AddArrayToObject(r, "supported_codecs");
    const char *list[] = SUPPORTED_CODECS;
    int n = sizeof(list)/sizeof(list[0]);
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(cc, cJSON_CreateString(list[i]));
    cJSON *f = cJSON_AddObjectToObject(r, "features");
    cJSON_AddBoolToObject(f, "pairing_button", true);
    cJSON_AddBoolToObject(f, "volume", true);
    cJSON_AddBoolToObject(f, "heartbeat", true);
    cJSON_AddBoolToObject(f, "ota_update", OTA_SUPPORTED);
    return r;
}

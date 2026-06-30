/*
 * Michi Music Stream Hi-Fi — firmware prototype.
 */
#include "app_core.h"
#include "config.h"
#include "pcm5122.h"

void app_main(void)
{
    pcm5122_init();
    app_core_init(DEVICE_TYPE, CONFIG_DEVICE_ID_STR, DEVICE_NAME);
}

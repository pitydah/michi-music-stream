/*
 * Michi Music Stream Standard — firmware prototype.
 */
#include "app_core.h"
#include "config.h"

void app_main(void)
{
    app_core_init(DEVICE_TYPE, CONFIG_DEVICE_ID_STR, DEVICE_NAME);
}

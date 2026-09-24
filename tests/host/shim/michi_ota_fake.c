#include <stdbool.h>

#include "michi_ota.h"
#include "michi_ota_fake.h"

static bool s_ota_busy = false;

bool michi_ota_busy(void)
{
    return s_ota_busy;
}

void michi_ota_fake_set_busy(bool busy)
{
    s_ota_busy = busy;
}

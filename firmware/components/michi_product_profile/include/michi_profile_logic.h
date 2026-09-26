#pragma once

#include <stdbool.h>
#include "michi_dac_types.h"

#ifdef __cplusplus
extern "C" {
#endif

michi_product_tier_t michi_profile_decide_tier(bool detected, michi_product_tier_t driver_tier);
bool michi_profile_decide_audio_available(michi_product_tier_t tier, bool initialized);

#ifdef __cplusplus
}
#endif

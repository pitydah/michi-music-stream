#include "michi_profile_logic.h"

michi_product_tier_t michi_profile_decide_tier(bool detected, michi_product_tier_t driver_tier)
{
    return detected ? driver_tier : MICHI_PRODUCT_DIAGNOSTIC;
}

bool michi_profile_decide_audio_available(michi_product_tier_t tier, bool initialized)
{
    return (tier == MICHI_PRODUCT_HIFI || tier == MICHI_PRODUCT_STANDARD) && initialized;
}

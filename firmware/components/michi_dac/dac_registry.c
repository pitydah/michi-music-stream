/*
 * Static DAC driver registry.
 *
 * Order is meaningful for autodetection: pcm512x (self-detectable, highest
 * priority), then mock (CI only, last so a real PCM512x always wins). The
 * PCM5102A is not self-detectable and can only be force-bound by profile.
 */

#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#include "michi_dac_types.h"

#include "dac_internal.h"

extern const michi_dac_driver_t g_michi_dac_pcm512x;
extern const michi_dac_driver_t g_michi_dac_pcm5102a;
extern const michi_dac_driver_t g_michi_dac_mock;

static const michi_dac_driver_t *s_drivers[MICHI_DAC_REGISTRY_MAX] = {
    &g_michi_dac_pcm512x,
    &g_michi_dac_pcm5102a,
#ifdef CONFIG_MICHI_DAC_MOCK
    &g_michi_dac_mock,
#endif
};

static const size_t s_driver_count =
    sizeof(s_drivers) / sizeof(s_drivers[0]);

const michi_dac_driver_t *michi_dac_registry_get(size_t index)
{
    if (index >= s_driver_count) {
        return NULL;
    }
    return s_drivers[index];
}

size_t michi_dac_registry_count(void)
{
    return s_driver_count;
}

const michi_dac_driver_t *michi_dac_registry_find_by_profile(const char *board_profile)
{
    if (board_profile == NULL || board_profile[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < s_driver_count; i++) {
        if (s_drivers[i]->board_profile != NULL &&
            strcmp(s_drivers[i]->board_profile, board_profile) == 0) {
            return s_drivers[i];
        }
    }
    return NULL;
}

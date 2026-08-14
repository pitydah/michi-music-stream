/*
 * Canonical product capability flags - THE single source of truth for the
 * boolean feature surface (MS-08 / P0-01 hardening).
 *
 * Both the discovery announce (michi_discovery) and GET /server/info
 * (michi_http canonical_json.c) read THIS table; no subsystem duplicates
 * a capability literal. A flag is true only while its handler is
 * implemented with a positive test:
 *  - session/heartbeat/volume: implemented (MS-07/MS-08);
 *  - diagnostics: implemented (its response shape is not frozen by the
 *    contract);
 *  - now_playing: the certified payload still answers 501 NOT_IMPLEMENTED;
 *  - ota: the A/B partitions exist, but the OTA service lands in phase 13
 *    (501).
 *
 * The announce carries ONLY the canonical group {heartbeat, session,
 * volume}; the extended flags (now_playing/diagnostics/ota) belong to
 * /server/info, not to the announce.
 *
 * Pure C (no ESP-IDF runtime dependency): the component and tests/host
 * compile the SAME source.
 */

#include "michi_product_profile.h"

static const michi_product_capabilities_t s_capabilities = {
    .session = true,
    .heartbeat = true,
    .volume = true,
    .now_playing = false,
    .diagnostics = true,
    .ota = false,
};

const michi_product_capabilities_t *michi_product_profile_capabilities(void)
{
    return &s_capabilities;
}

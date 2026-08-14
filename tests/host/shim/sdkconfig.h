#pragma once
/* Shim for host-side tests: the Kconfig values the pairing component
 * compiles against (mirrors firmware/components/michi_pairing/Kconfig
 * defaults). TEST-ONLY: never compiled into firmware. */

#define CONFIG_MICHI_PAIRING_WINDOW_SECONDS 120
#define CONFIG_MICHI_PAIRING_MAX_CONTROLLERS 8

/* Deterministic debouncer implementation (PAIR-BTN-01). See
 * michi_button_debounce.h for the contract. Pure: no GPIO, no RTOS, no time
 * source of its own - the caller feeds samples + monotonic timestamps. */

#include <stddef.h>

#include "michi_button_debounce.h"

void michi_button_debounce_init(michi_button_debounce_t *d, uint32_t debounce_ms)
{
    if (d == NULL) {
        return;
    }
    d->debounce_ms = debounce_ms;
    d->stable_level = 1;
    d->candidate_level = 1;
    d->candidate_since_us = 0;
    d->stable_since_us = 0;
    d->samples_fed = 0;
    d->level_changes = 0;
    d->stable_presses = 0;
    d->stable_releases = 0;
}

michi_button_debounce_evt_t michi_button_debounce_feed(michi_button_debounce_t *d,
                                                       int raw_level,
                                                       int64_t now_us)
{
    if (d == NULL) {
        return MICHI_BTN_DEBOUNCE_NONE;
    }

    d->samples_fed++;

    if (raw_level != d->candidate_level) {
        /* New candidate level observed: (re)start the stable window. A bounce
         * that reverts before debounce_ms expires cancels here. */
        d->candidate_level = raw_level;
        d->candidate_since_us = now_us;
        d->level_changes++;
    }

    /* Already stable: no work, no transition. */
    if (d->candidate_level == d->stable_level) {
        return MICHI_BTN_DEBOUNCE_NONE;
    }

    /* Candidate differs from stable: wait for the full debounce window at the
     * candidate level before confirming. */
    if ((now_us - d->candidate_since_us) >= (int64_t)d->debounce_ms * 1000) {
        d->stable_level = d->candidate_level;
        d->stable_since_us = now_us;
        if (d->stable_level == 0) {
            d->stable_presses++;
            return MICHI_BTN_DEBOUNCE_PRESS;
        }
        d->stable_releases++;
        return MICHI_BTN_DEBOUNCE_RELEASE;
    }

    return MICHI_BTN_DEBOUNCE_NONE;
}

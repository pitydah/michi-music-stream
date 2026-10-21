/* Receiver operational authority - see include/michi_ops_gate.h.
 *
 * Pure C11: no ESP-IDF, no FreeRTOS, no logging. Every layer (capability
 * tables, HTTP, session, audio, OTA, app_main) includes this, and the host
 * tests drive the same code the firmware runs.
 */

#include "michi_ops_gate.h"

#include <stdatomic.h>
#include <stddef.h>

/* Zero == MICHI_OPS_BOOT_PENDING, so static initialisation already denies
 * playback before michi_ops_init() ever runs. */
static atomic_int s_ops = MICHI_OPS_BOOT_PENDING;

#ifdef MICHI_HOST_TEST
/* Host-only instrumentation: lets a test widen the validate -> swap window so a
 * lost update becomes observable deterministically. Never compiled into the
 * firmware. */
static void (*s_test_between_validate_and_swap)(void);
void test_ops_set_between_validate_and_swap(void (*fn)(void))
{
    s_test_between_validate_and_swap = fn;
}
#endif

const char *michi_ops_state_name(michi_ops_state_t state)
{
    switch (state) {
    case MICHI_OPS_BOOT_PENDING:
        return "boot_pending";
    case MICHI_OPS_READY:
        return "ready";
    case MICHI_OPS_DIAGNOSTIC:
        return "diagnostic";
    case MICHI_OPS_RESTRICTED:
        return "restricted";
    case MICHI_OPS_FATAL:
        return "fatal";
    case MICHI_OPS_UPDATING:
        return "updating";
    default:
        return "unknown";
    }
}

void michi_ops_init(void)
{
    atomic_store(&s_ops, MICHI_OPS_BOOT_PENDING);
}

michi_ops_state_t michi_ops_get(void)
{
    return (michi_ops_state_t)atomic_load(&s_ops);
}

bool michi_ops_playback_allowed(void)
{
    return michi_ops_get() == MICHI_OPS_READY;
}

bool michi_ops_ota_allowed(void)
{
    const michi_ops_state_t s = michi_ops_get();
    return s == MICHI_OPS_READY || s == MICHI_OPS_DIAGNOSTIC;
}

static bool ops_transition_valid(michi_ops_state_t from, michi_ops_state_t to)
{
    if (from == to) {
        return true;
    }
    /* Terminal: nothing good comes from resurrecting a unit whose essential
     * subsystem failed without a reboot. */
    if (from == MICHI_OPS_FATAL) {
        return false;
    }
    /* The boot decision is a one-way door. */
    if (to == MICHI_OPS_BOOT_PENDING) {
        return false;
    }
    if (from == MICHI_OPS_BOOT_PENDING) {
        return true;
    }
    /* Tightening is always allowed. */
    if (to == MICHI_OPS_RESTRICTED || to == MICHI_OPS_FATAL) {
        return true;
    }
    if (from == MICHI_OPS_UPDATING) {
        /* The update finished: back to normal service. */
        return to == MICHI_OPS_READY || to == MICHI_OPS_DIAGNOSTIC;
    }
    if (to == MICHI_OPS_UPDATING) {
        return true;
    }
    /* Leaving RESTRICTED requires michi_ops_recover(). */
    if (from == MICHI_OPS_RESTRICTED) {
        return false;
    }
    /* READY <-> DIAGNOSTIC: audio availability changed at runtime. */
    return (from == MICHI_OPS_READY && to == MICHI_OPS_DIAGNOSTIC) ||
           (from == MICHI_OPS_DIAGNOSTIC && to == MICHI_OPS_READY);
}

int michi_ops_set(michi_ops_state_t state)
{
    if (state < MICHI_OPS_BOOT_PENDING || state > MICHI_OPS_UPDATING) {
        return -1;
    }
    /* Compare-and-swap, not read-validate-write: two tasks tightening and
     * relaxing at the same time must not be able to lose the stronger verdict
     * (e.g. a FATAL overwritten by a relax that validated against a stale
     * READY). On failure the CAS hands back the value that won, and the
     * transition is re-validated against it. */
    int expected = atomic_load(&s_ops);
    for (;;) {
        if (!ops_transition_valid((michi_ops_state_t)expected, state)) {
            return -2;
        }
#ifdef MICHI_HOST_TEST
        if (s_test_between_validate_and_swap != NULL) {
            s_test_between_validate_and_swap();
        }
#endif
        if (atomic_compare_exchange_weak(&s_ops, &expected, (int)state)) {
            return 0;
        }
        /* expected now holds the current value; loop and re-validate. */
    }
}

int michi_ops_recover(michi_ops_state_t target)
{
    if (target != MICHI_OPS_READY && target != MICHI_OPS_DIAGNOSTIC) {
        return -1;
    }
    /* Atomic exit: only succeeds if the gate is RESTRICTED at the instant of
     * the swap, so a concurrent tightening cannot be overwritten. */
    int expected = MICHI_OPS_RESTRICTED;
    if (atomic_compare_exchange_strong(&s_ops, &expected, (int)target)) {
        return 0;
    }
    return -2;
}

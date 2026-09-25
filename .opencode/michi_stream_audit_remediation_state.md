# Michi Stream KILLCRITIC — Round 3

START_HEAD: a44ea3c803a2bd62cfac772cd9d1463f80523c5c
LAST_IMPLEMENTATION_HEAD: fba00bc9581561009efb4ca6d588523c914c62c3
LAST_VERIFIED_HEAD: fba00bc9581561009efb4ca6d588523c914c62c3
BRANCH: fix/ui-device-gaps
WORKTREE_STATUS: clean

GLOBAL_STATUS: IN_PROGRESS

| Phase | Status | Reproduced | Test before patch | Patch | Falsified | Firmware | Commit |
|---|---|---|---|---|---|---|---|
| R3-00 | PASS | YES | YES | N/A | YES | FAIL (reproduced) | - |
| R3-01 | PASS | YES | YES | YES | YES | PASS | c653914 |
| R3-02 | PASS | YES | YES | YES | YES | PASS | 3276953 |
| R3-03 | PASS | YES | YES | YES | YES | PASS | 824815e |
| R3-04 | PASS | YES | YES | YES | YES | PASS | e985918 |
| R3-05 | PASS | YES | YES | YES | YES | PASS | fba00bc |
| R3-06 | TODO | NO | NO | NO | NO | NO | - |
| R3-07 | TODO | NO | NO | NO | NO | NO | - |
| R3-08 | TODO | NO | NO | NO | NO | NO | - |
| R3-09 | TODO | NO | NO | NO | NO | NO | - |
| R3-10 | TODO | NO | NO | NO | NO | NO | - |
| R3-11 | TODO | NO | NO | NO | NO | NO | - |
| R3-12 | TODO | NO | NO | NO | NO | NO | - |
| R3-13 | TODO | NO | NO | NO | NO | NO | - |
| R3-14 | TODO | NO | NO | NO | NO | NO | - |
| R3-15 | TODO | NO | NO | NO | NO | NO | - |
| R3-16 | TODO | NO | NO | NO | NO | NO | - |
| R3-17 | TODO | NO | NO | NO | NO | NO | - |
| R3-18 | TODO | NO | NO | NO | NO | NO | - |
| R3-19 | TODO | NO | NO | NO | NO | NO | - |
| R3-20 | TODO | NO | NO | NO | NO | NO | - |
| R3-21 | TODO | NO | NO | NO | NO | NO | - |
| R3-22 | TODO | NO | NO | NO | NO | NO | - |
| R3-23 | TODO | NO | NO | NO | NO | NO | - |
| R3-24 | TODO | NO | NO | NO | NO | NO | - |
| R3-25 | TODO | NO | NO | NO | NO | NO | - |

## Pre-flight Evidence (R3-00)
- Branch: fix/ui-device-gaps
- Head: a44ea3c803a2bd62cfac772cd9d1463f80523c5c
- Working tree: clean
- Host tests: make -C tests/host clean && make -C tests/host test -> PASS
- E2E suite: python3 tests/e2e/run_e2e.py -> PASS (13/13)
- Cppcheck: 47/47 files checked -> PASS (0 warnings)
- Firmware build: docker run ... espressif/idf:release-v5.3 -> FAIL (reproduced error in app_main.c line 521: "/*" within comment, line 536: 'st_res' undeclared)

## R3-01 Evidence
- Defect: Unterminated block comment at app_main.c:489 causing compiler error: "/*" within comment and undeclared st_res.
- Reproduction: ESP-IDF release-v5.3 docker build failed at [1233/1253].
- Patch: Closed block comment at app_main.c:489 before char dac_prof[64].
- Verification: ESP-IDF release-v5.3 docker build passed 100%, binary size 1626656 bytes (<= 4194304), sdkconfig assertions pass, host tests pass, cppcheck passes with 0 warnings.

## R3-02 Evidence
- Defect: Pairing and discovery worker tasks relied on volatile bool pending flags described as "atomic" which could race and lose events.
- Patch: Replaced volatile flags with FreeRTOS native task notification bits (PAIRING_NOTIFY_EXPIRED, PAIRING_NOTIFY_STOP, DISCOVERY_NOTIFY_TICK, DISCOVERY_NOTIFY_TIME_SYNC, DISCOVERY_NOTIFY_STOP). Implemented xTaskNotify, xTaskNotifyFromISR, and xTaskNotifyWait in host task shim.
- Verification: Host tests test_michi_pairing and test_discovery_disc pass 100%. ESP-IDF release-v5.3 docker firmware build passes 100%. Cppcheck passes with 0 warnings.

## R3-03 Evidence
- Defect: Previous tests QUEUE-PAIR-01 and QUEUE-DISC-01 did not guarantee worker pressure or queue saturation, relying on misleading names and timing.
- Patch: Added deterministic synchronization test hooks in pairing (michi_pairing_test_lock, michi_pairing_test_unlock, michi_pairing_test_is_window_open_locked, michi_pairing_test_notify_expired) and discovery (michi_discovery_test_lock, michi_discovery_test_unlock, michi_discovery_test_is_timer_active, michi_discovery_test_is_active, michi_discovery_test_notify_tick).
- Falsification Tests:
  - EVENT-COALESCE-PAIR-01: Locks pairing mutex, advances clock past window expiry, fires 20 expired notifications while worker is blocked, verifies worker blocked & window remains open, unlocks mutex, verifies worker unblocks, closes window, and posts MICHI_EVENT_PAIRING_WINDOW_CLOSED without permanent loss.
  - EVENT-COALESCE-DISC-01: Locks announce mutex, fires 20 announce ticks while worker is blocked, verifies no announce sent while contention persists, unlocks mutex, verifies worker emits announce. Proves discovery invariant: while discovery active, event processing cannot leave timer inactive and worker idle forever (verifies timer rearmed & active, advances past next period and verifies subsequent announce emitted).
  - EVENT-COALESCE-DISC-02: Locks announce mutex, fires 5 rapid SNTP time syncs while worker is blocked, verifies no packet emitted while locked, unlocks mutex, verifies announce emitted on coalesced sync.
- Verification: Host tests pass 100%. Cppcheck 47/47 files clean (0 warnings). ESP-IDF release-v5.3 docker firmware build passes 100% (binary size 1626800 bytes <= 4194304).

## R3-04 Evidence
- Defect: Pairing and discovery worker tasks could be accessed or notified after teardown; join timeout during shutdown destroyed resources leaving corrupted state; subsequent shutdown retry could double-notify dead task handle; time sync callback could fire into discovery during/after teardown.
- Invariant & Implementation:
  - Synchronized worker lifecycle state machine (`michi_worker_lifecycle_t`: `MICHI_WORKER_STOPPED`, `MICHI_WORKER_RUNNING`, `MICHI_WORKER_STOP_REQUESTED`, `MICHI_WORKER_EXITED`) under critical section `s_lifecycle_mux`.
  - Worker tasks manage their own exit lifecycle: mark `MICHI_WORKER_EXITED`, clear `s_task = NULL`, signal binary done semaphore, and call `vTaskDelete(NULL)`. No external `vTaskDelete` is performed.
  - Shutdown join timeout preserves resources and returns `ESP_ERR_TIMEOUT`, maintaining retriable state.
  - Subsequent retry recognizes `s_worker_state == MICHI_WORKER_EXITED`, skips notifying the dead task handle, drains semaphore, and tears down authoritatively.
  - Discovery shutdown unregisters SNTP time sync callback immediately before teardown.
  - Host task shim instrumented with task lifecycle states, `pthread_mutex_t`-backed critical sections, and diagnostic counters (`test_task_invalid_notify_count()`, `test_task_external_delete_count()`).
- Falsification Tests:
  - `PAIR-LIFE-01..04`: Normal stop, delayed worker exit joining within timeout, join timeout preserving state followed by clean retry without notifying dead handle, and repeated idempotent shutdown.
  - `DISC-LIFE-01..05`: Normal stop, delayed worker exit joining within timeout, join timeout preserving state followed by clean retry without notifying dead handle, repeated idempotent shutdown, and late SNTP time sync callback after shutdown without notify to dead task.
- Verification: Host tests pass 100%. Cppcheck 47/47 files clean (0 warnings). ESP-IDF release-v5.3 docker firmware build passes 100% (binary size 1627040 bytes <= 4194304, SPIRAM OCT 16MB verified).

## R3-05 Evidence
- Invariants Enforced:
  - `PAIR-INV-01`: No callback may act after teardown begins (esp_timer_stop + generation bump + lifecycle state check in window_timer_cb prevents timer callback execution during/after shutdown).
  - `PAIR-INV-02`: No worker access after mutex deletion (worker exit ownership confirmed and task handle cleared before mutex deletion; timeout preserves mutex while worker alive).
  - `PAIR-INV-03`: No stale TaskHandle_t notification (worker state EXITED clears task handle under lifecycle mux; notifications to retired tasks prevented).
  - `PAIR-INV-04`: Join timeout leaves subsystem retryable without leaking/destroying resources, permitting clean subsequent retry.
  - `PAIR-INV-05`: `PAIRING_WINDOW_CLOSED` is never double-posted (closed status checked under mutex; shutdown closes window without posting event; manual close or late expiry on closed window exits early).
  - `PAIR-INV-06`: PIN display callback invoked with `NULL` under mutex prior to resource teardown to clear screen PIN; callback function pointer and context cleared to `NULL` so late calls never invoke invalid targets.
- Implementation:
  - Cleaned up shutdown sequence in `firmware/components/michi_pairing/michi_pairing.c`: invoke `pin_display_notify(NULL)` before deleting mutex, zero out `s_pin_display_cb` and `s_pin_display_ctx` under mutex.
  - Added test hook `michi_pairing_test_has_mutex()`.
- Falsification Tests:
  - Added `test_pair_inv_01_no_callback_after_teardown`
  - Added `test_pair_inv_02_no_worker_access_after_mutex_delete`
  - Added `test_pair_inv_03_no_stale_task_notification`
  - Added `test_pair_inv_04_timeout_leaves_retryable`
  - Added `test_pair_inv_05_no_double_post_closed`
  - Added `test_pair_inv_06_pin_display_cb_lifecycle`
- Verification: Host tests pass 100% (including all PAIR-INV-01..06). Cppcheck 47/47 clean (0 warnings). ESP-IDF release-v5.3 docker firmware build passes 100% (binary size 1627040 bytes <= 4194304).


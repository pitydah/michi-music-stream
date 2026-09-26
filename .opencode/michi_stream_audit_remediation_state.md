# Michi Stream KILLCRITIC — Round 3

START_HEAD: a44ea3c803a2bd62cfac772cd9d1463f80523c5c
LAST_IMPLEMENTATION_HEAD: c93dca6fb7bee181cbeb7f629a46abc1ff37155a
LAST_VERIFIED_HEAD: c93dca6fb7bee181cbeb7f629a46abc1ff37155a
BRANCH: fix/ui-device-gaps
WORKTREE_STATUS: clean

GLOBAL_STATUS: IN_PROGRESS
PRE_MERGE_CLOSURE_STATUS: IN_PROGRESS

## PRE-MERGE CLOSURE WAVES
- WAVE_A: PASS (Close lifecycle/SMP residuals: A1..A7)
- WAVE_B: REGRESSION
- WAVE_C: REGRESSION
- WAVE_D: REGRESSION

# FINAL SOFTWARE CLOSURE

START_HEAD: 690f648deb9a56a8b7faef8c18ccd910f5c27ebd
CURRENT_IMPLEMENTATION_HEAD: 690f648deb9a56a8b7faef8c18ccd910f5c27ebd
FINAL_CERTIFICATION_HEAD: PENDING
BRANCH: fix/ui-device-gaps
PRE_MERGE_CLOSURE_STATUS: IN_PROGRESS

| Phase | Status | Reproduced | Test | Firmware | Static | Commit |
|---|---|---|---|---|---|---|
| F0 | PASS | YES | N/A | PASS | PASS | - |
| F1 | PASS | YES | YES | PASS | PASS | 41541c8 |
| F2 | PASS | YES | YES | PASS | PASS | 41541c8 |
| F3 | PASS | YES | YES | PASS | PASS | 41541c8 |
| F4 | PASS | YES | YES | PASS | PASS | 41541c8 |
| F5 | PASS | YES | YES | PASS | PASS | 9514b3d |
| F6 | PASS | YES | YES | PASS | PASS | 9514b3d |
| F7 | PASS | YES | YES | PASS | PASS | pending_commit |
| F8 | TODO | - | - | - | - | - |
| F9 | TODO | - | - | - | - | - |
| F10 | TODO | - | - | - | - | - |
| F11 | TODO | - | - | - | - | - |
| F12 | TODO | - | - | - | - | - |
| F13 | TODO | - | - | - | - | - |
| F14 | TODO | - | - | - | - | - |
| F15 | TODO | - | - | - | - | - |
| F16 | TODO | - | - | - | - | - |
| F17 | TODO | - | - | - | - | - |
| F18 | TODO | - | - | - | - | - |

### Phase F0: Complete Shared-State Inventory — Audio Output

| Variable | Owner | Writers | Readers | Synchronization | Lifetime |
|---|---|---|---|---|---|
| `s_ring.buf` | Lifecycle (`init`/`deinit`) | `init()` (alloc), `deinit()` (free) | `ring_write()`, `ring_read()` | Allocated before task, freed after task dead | Valid while `s_state != UNINITIALIZED` |
| `s_ring.size` | Lifecycle (`init`/`deinit`) | `init()` | `ring_write()`, `ring_read()` | Immutable after `init()` | Valid while initialized |
| `s_ring.head` | Producer | `ring_write()`, `init()`, `start()`, `flush()`, `i2s_task` (quiesce) | `ring_write()` | `s_ring_lock` (portMUX critical section) | Valid while initialized |
| `s_ring.tail` | Consumer (`i2s_task`) | `ring_read()`, `i2s_task` (quiesce), `init()`, `start()`, `flush()` | `ring_read()` | `s_ring_lock` (portMUX critical section) | Valid while initialized |
| `s_ring.used` | Shared counter | `ring_write()`, `ring_read()`, `flush()`, `init()`, `start()`, `quiesce` | `ring_used()` (`ring_write`, `ring_read`, `i2s_task` prefill) | `s_ring_lock` (portMUX critical section) | Valid while initialized |
| `s_tx` | Lifecycle (`init`/`deinit`) | `init()` (`i2s_new_channel`), `deinit()` (`NULL`) | `start()` (`enable`), `stop()` (`disable`), `deinit()` (`del`), `i2s_task` (`write`) | Driver handle; created at init, destroyed at deinit; access sequenced by lifecycle state | `init()` to `deinit()` |
| `s_task` | Lifecycle (`start`/`stop`) | `start()` (stores handle), `stop()` (clears `NULL`) | `ring_write()`, `send_cmd_and_wait_ack()`, `stop()`, `deinit()` | `s_state_lock` (portMUX critical section) | Valid from `start()` until worker exit join |
| `s_inited` | Lifecycle (`init`/`deinit`) | `init()`, `deinit()` | `init()`, `start()`, `quiesce()`, `resume()`, `stop()`, `deinit()` | `s_state_lock` (portMUX critical section) | Entire process lifetime |
| `s_running` | Lifecycle / State | `start()`, `stop()` | `write()`, `is_running()`, `flush()` | Consolidated into `s_state` under `s_state_lock` | Entire process lifetime |
| `s_run` | Lifecycle / State | `start()` (`true`), `stop()` (`false`) | `i2s_task` loop termination condition | `s_state_lock` (portMUX critical section) | `start()` to `stop()` |
| `s_task_done` | Consumer worker | `i2s_task` (sets `true` before self-delete), `start()`/`stop()` (clears `false`) | `stop()` join wait loop, `deinit()` | `s_state_lock` (portMUX critical section) | Task execution lifetime |
| `s_consumer_sleeping` | Consumer worker | `i2s_task` | `ring_write()` | Removed bare data race; synchronized / lifecycle-safe wake | Task execution lifetime |
| `s_state` | Audio output state machine | `init()`, `start()`, `quiesce()`, `resume()`, `stop()`, `deinit()`, `i2s_task` | `start()`, `write()`, `quiesce()`, `resume()`, `is_quiesced()`, `stop()`, `get_state()`, `i2s_task` | `s_state_lock` (portMUX critical section) for all reads & writes | Entire process lifetime |
| `s_pending_cmd` | Command dispatcher | `send_cmd_and_wait_ack()` | `handle_pending_command_in_task()` | `s_state_lock` (portMUX critical section) + `s_cmd_mux` | `init()` to `deinit()` |
| `s_cmd_result` | Consumer worker | `handle_pending_command_in_task()` | `send_cmd_and_wait_ack()` | `s_state_lock` (portMUX critical section) | `init()` to `deinit()` |
| `s_cmd_generation` | Command dispatcher | `send_cmd_and_wait_ack()` | `handle_pending_command_in_task()`, `send_cmd_and_wait_ack()` | `s_state_lock` (portMUX critical section) | `init()` to `deinit()` |
| `s_cmd_ack_generation` | Consumer worker | `handle_pending_command_in_task()` | `send_cmd_and_wait_ack()` | `s_state_lock` (portMUX critical section) | `init()` to `deinit()` |
| `s_cmd_mux` | Command dispatcher | `init()` (create), `deinit()` (delete) | `send_cmd_and_wait_ack()` | FreeRTOS Mutex semaphore | `init()` to `deinit()` |
| `s_cmd_ack_sem` | Command dispatcher | `init()` (create), `deinit()` (delete) | `handle_pending_command_in_task()` (`give`), `send_cmd_and_wait_ack()` (`take`) | FreeRTOS Binary semaphore | `init()` to `deinit()` |
| `s_prefill_bytes` | Config | `init()` | `i2s_task` | Immutable after `init()` | `init()` to `deinit()` |
| `s_bit_depth` | Config | `init()` | `i2s_task` | Immutable after `init()` | `init()` to `deinit()` |
| `s_chunk` | Consumer worker (`i2s_task`) | `i2s_task` | `i2s_task` | Single-owner task: no other task or caller touches or reads `s_chunk` | Static BSS |
| `s_error_count` | Diagnostics | `i2s_task`, `stop()` | `michi_audio_output_get_error_count()` | `s_err_lock` (portMUX critical section) | Entire process lifetime |

### Phase F1-F4 Evidence: Audio Output Hardening

- **F1 (Audio Init False Success):**
  - Defect: When semaphore creation failed in `michi_audio_output_init`, `err` held `ESP_OK` from prior calls, jumping to `fail_ring` and returning `ESP_OK` without allocating resources.
  - Fix: Explicit `err = ESP_ERR_NO_MEM;` on mutex or binary semaphore creation failure before `goto fail_ring;`.
  - Tests: `AUDIO-INIT-FAIL-01..05` (5/5 PASS).
- **F2 (State Machine Matrix):**
  - Defect: `quiesce()` allowed forcing `QUIESCED` state from `INITIALIZED` or `STOPPED` without a live worker task.
  - Fix: Explicit transition matrix enforced under `s_state_lock`. Illegal edges (`INITIALIZED->QUIESCED`, `STOPPED->QUIESCED`, etc.) rejected with `ESP_ERR_INVALID_STATE`.
  - Tests: `AUDIO-STATE-01..08` (8/8 PASS).
- **F3 (Shared-State Synchronization):**
  - Defect: Bare flag `s_consumer_sleeping` had unsynchronized data race between consumer and producer. FreeRTOS APIs and logging were invoked under spinlocks.
  - Fix: Removed `s_consumer_sleeping` completely. All state variables unified under `s_state_lock`. Zero FreeRTOS calls, zero logging under spinlock. Snapshots taken under lock, FreeRTOS/logging invoked strictly outside critical sections.
- **F4 (Command Protocol & Generations):**
  - Defect: Stale ACKs could resolve future commands; timeouts left state undefined; commands could block on dead workers.
  - Fix: Monotonic command generation `s_cmd_generation` checked on ACK (`s_cmd_ack_generation`). Dead worker check rejects immediately. Command timeout transitions pipeline to `FAULTED`. Recovery via `stop()` restores clean state.
  - Tests: `AUDIO-CMD-01..06` (6/6 PASS).

### Phase F5-F6 Evidence: Session Pause/Resume & Teardown Truth

- **F5 (Pause/Resume Error Propagation & Multi-field Atomicity):**
  - Defect: `michi_audio_session_set_paused(bool)` had void return type, unconditionally updating `s_paused` and ignoring underlying quiesce/resume failure. In multi-field patches (`volume` + `paused`), volume was applied before pause/resume, leaving volume mutated even if pause failed.
  - Fix: `michi_audio_session_set_paused` changed to return `esp_err_t`, committing `s_paused` only if quiesce/resume succeeds. In `michi_session_patch()`, fallible pause/resume executes FIRST. If it fails, mutation aborts immediately without modifying volume, without changing `s_session.info.paused`, and without posting FSM state events.
  - Tests: `SESSION-PAUSE-01`, `SESSION-PAUSE-02`, `SESSION-RESUME-01`, `SESSION-RESUME-02`, `SESSION-PATCH-ATOMIC-01` (all PASS).
- **F6 (Audio Teardown Revert on Stop Failure):**
  - Defect: If `michi_audio_session_stop()` failed during teardown, session state remained set to `STOPPING` instead of rolling back to the previous state.
  - Fix: In `session_teardown_locked()`, save `prev_state` and restore `s_session.info.state = prev_state` if `michi_audio_session_stop()` returns error.
  - Tests: `SESSION-STOP-FAIL-01` (PASS).

### Phase F7 Evidence: HTTP Body Deadline Strict Enforcement

- **F7 (HTTP Body Total Deadline Gate):**
  - Defect: In `michi_http_read_body()`, the anti-slowloris total deadline check was only performed at the beginning of each loop iteration. A client trickling data whose final chunk crossed the 2000ms deadline would exit the loop because `received == content_len` and return `ESP_OK`, evading the timeout policy.
  - Fix: Check `esp_timer_get_time() >= deadline_us` immediately after adding received bytes, as well as unconditionally after loop completion. If the total transfer elapsed >= 2000ms, abort immediately with `ESP_ERR_TIMEOUT`.
  - Tests: `HTTP-SLOW-01..04` (including `HTTP-SLOW-04` specifically asserting that a transfer completing on a deadline-exceeding chunk is rejected with `ESP_ERR_TIMEOUT`).



| Phase | Status | Reproduced | Test before patch | Patch | Falsified | Firmware | Commit |
|---|---|---|---|---|---|---|---|
| R3-00 | PASS | YES | YES | N/A | YES | FAIL (reproduced) | - |
| R3-01 | PASS | YES | YES | YES | YES | PASS | c653914 |
| R3-02 | PASS | YES | YES | YES | YES | PASS | 3276953 |
| R3-03 | PASS | YES | YES | YES | YES | PASS | 824815e |
| R3-04 | PASS | YES | YES | YES | YES | PASS | e985918 |
| R3-05 | PASS | YES | YES | YES | YES | PASS | fba00bc |
| R3-06 | PASS | YES | YES | YES | YES | PASS | ad47c89 |
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

## R3-06 Evidence
- Discovery Shutdown Architecture:
  1. `michi_time_register_sync_cb(NULL, NULL)` called immediately at shutdown start to detach external SNTP callback.
  2. `esp_timer_stop(s_announce_timer)` and generation bump prevents timer work.
  3. Lifecycle state transitions to `MICHI_WORKER_STOP_REQUESTED`, sends `DISCOVERY_NOTIFY_STOP` to worker task.
  4. Cooperative join on `s_discovery_done_sem` with 1000ms timeout. If timed out, preserves mutex, timer, socket, active state, and returns `ESP_ERR_TIMEOUT` without destructive cleanup.
  5. Worker confirms exit, marks `MICHI_WORKER_EXITED`, clears `s_discovery_task = NULL`, signals done semaphore, and calls `vTaskDelete(NULL)`.
  6. Caller acquires sole exclusive ownership under `s_announce_mutex`, tears down socket, retires mDNS, deletes timer and mutex.
- Invariants & Tests (`DISC-LIFE-01..07`):
  - `DISC-LIFE-01`: Normal shutdown lifecycle transitions.
  - `DISC-LIFE-02`: Worker/mutex contention when stop begins resolves cleanly.
  - `DISC-LIFE-03`: First shutdown timeout preserves socket, mutex, timer, and active resources.
  - `DISC-LIFE-04`: Worker exit after timeout allows clean retry without double notification or leaks.
  - `DISC-LIFE-05`: Time-sync callback after shutdown cannot access discovery.
  - `DISC-LIFE-06`: Timer callback racing shutdown cannot access destroyed state.
  - `DISC-LIFE-07`: Repeated shutdown is idempotent.
- Test Hooks Isolated:
  - Added `#ifdef MICHI_HOST_TEST` guards in `michi_discovery.h` and `michi_discovery.c` for test hooks (`has_mutex`, `has_timer`, `socket_fd`).
- Verification: Host tests pass 100% (including DISC-LIFE-01..07). Cppcheck 47/47 files clean (0 warnings). ESP-IDF release-v5.3 docker firmware build passes 100% (binary size 1627040 bytes <= 4194304, SPIRAM OCT 16MB verified).

## R3-06.5 Evidence (Lifecycle & SMP Hardening)
- Scope Hardened:
  - `firmware/components/michi_pairing/michi_pairing.c`
  - `firmware/components/michi_discovery/michi_discovery.c`
  - `tests/host/shim/freertos/FreeRTOS.h`, `task.h`, `task.c`, `semphr.h`
  - `tests/host/test_michi_pairing.c`
  - `tests/host/test_discovery_disc.c`
- Defects Addressed & Invariants Enforced:
  1. FreeRTOS SMP / Spinlock Safety:
     - FreeRTOS APIs (`xTaskNotify`) removed from inside `portENTER_CRITICAL(&s_lifecycle_mux)`.
     - Host FreeRTOS shim instrumented to measure critical section depth (`s_freertos_critical_depth`) and detect FreeRTOS API calls inside critical sections (`test_freertos_api_in_critical_count()`).
     - Falsification reproduced: unpatched code caught calling `xTaskNotify` with depth=1.
     - Patched with notification lease counter (`s_notify_inflight`) under spinlock; task notify executed outside spinlock; worker cooperatively drains in-flight notifies before exiting.
  2. Worker self-deletion and notify race prevention:
     - Worker drains `s_notify_inflight == 0` before transitioning to `MICHI_WORKER_EXITED` and calling `vTaskDelete(NULL)`.
     - No external task delete allowed (`test_task_external_delete_count() == 0`).
     - Zero invalid task notifies to dead tasks (`test_task_invalid_notify_count() == 0`).
  3. Public API admission and teardown safety:
     - Added `pairing_api_enter()`/`pairing_api_exit()` and `discovery_api_enter()`/`discovery_api_exit()`.
     - Only admits callers when `s_initialized && s_worker_state == RUNNING`. Rejects with `ESP_ERR_INVALID_STATE` (or `ESP_OK` for idempotent stop) during shutdown and teardown.
     - Shutdown tracks `s_api_inflight` and drains all in-flight API calls before destroying mutexes/timers/sockets.
     - Concurrent `shutdown()` callers serialized via `s_shutdown_in_progress` without timeouts or race conditions.
  4. Tests Added:
     - `test_michi_pairing.c`: `PAIR-CONCUR-01..04` + final invariants (`test_freertos_api_in_critical_count() == 0`, `test_task_invalid_notify_count() == 0`, `test_task_external_delete_count() == 0`).
     - `test_discovery_disc.c`: `DISC-CONCUR-01..05` + final invariants (`test_freertos_api_in_critical_count() == 0`, `test_task_invalid_notify_count() == 0`, `test_task_external_delete_count() == 0`).
- Verification:
  - Host unit tests: 100% pass (`make -C tests/host clean && make -C tests/host test`).
  - Static analysis: Cppcheck 47/47 files clean (0 warnings, 0 errors).
  - ESP-IDF release-v5.3 docker firmware build: 100% pass (`michi-music-stream.bin` size: 1627792 bytes <= 4194304, SPIRAM OCT 16MB verified).

## Wave A Evidence (Lifecycle & SMP Residuals: A1..A7)
- Scope Hardened:
  - `firmware/components/michi_pairing/include/michi_pairing.h`, `michi_pairing.c`
  - `firmware/components/michi_discovery/include/michi_discovery.h`, `michi_discovery.c`
  - `tests/host/test_michi_pairing.c`
  - `tests/host/test_discovery_disc.c`
- Invariants & Improvements:
  1. `LIFE-SMP-01`: Bounded shutdown wait for concurrent callers with `MICHI_LIFECYCLE_SHUTDOWN_TIMEOUT_MS = 1000`. Exits cleanly with `ESP_ERR_TIMEOUT` on expiry.
  2. `LIFE-SMP-02`: Bounded API drain wait with `MICHI_LIFECYCLE_API_DRAIN_TIMEOUT_MS = 500`. On drain expiry, preserves all resources (mutexes, timers, sockets), logs `ESP_LOGE`, leaves retriable state (`STOP_REQUESTED`), and returns `ESP_ERR_TIMEOUT`.
  3. `LIFE-SMP-03`: Bounded drain timeout verified retriable: subsequent shutdown invocation after API drain completes succeeds cleanly and tears down all resources without leaks.
  4. `LIFE-SMP-04`: FreeRTOS SMP correctness: removed `volatile` from lifecycle/task flags (`s_worker_state`, `s_initialized`, `s_shutdown_in_progress`, `s_notify_inflight`, `s_api_inflight`) in production, replaced test stress flags with C11 `<stdatomic.h>`.
  5. `LIFE-SMP-05`: Single authority for `s_worker_state` under `s_lifecycle_mux`, eliminating bare reads/writes.
  6. `LIFE-SMP-06`: Documented explicit lock order contract in file headers: Level 1 `s_lifecycle_mux` (spinlock) -> Level 2 `s_mutex` / `s_announce_mutex` (mutex) -> Level 3 (external callbacks/IO).
  7. `LIFE-SMP-07`: `pin_display_notify()` and `set_pin_display_cb()` read `s_initialized` under `s_lifecycle_mux`.
  8. `LIFE-SMP-08`: Public API admission rejected when `s_worker_state` is `STOP_REQUESTED`, `EXITED`, or `STOPPED`.
- Tests Added:
  - `test_michi_pairing.c`: `test_life_smp_02_03_api_drain_timeout_and_retry`
  - `test_discovery_disc.c`: `test_life_smp_disc_02_03_api_drain_timeout_and_retry`
- Verification:
  - Host test suite: 100% pass (`make -C tests/host clean && make -C tests/host test`).
  - Static analysis: Cppcheck clean (0 warnings, 0 errors).
  - ESP-IDF release-v5.3 docker firmware build: 100% pass (`michi-music-stream.bin` size: 1627792 bytes <= 4194304).

## Wave B Evidence (Audio Output Single-Owner & Quiesce: B1..B10)
- Scope Hardened:
  - `firmware/components/michi_audio_output/include/michi_audio_output.h`
  - `firmware/components/michi_audio_output/michi_audio_output.c`
  - `tests/host/shim/driver/i2s_std.h`
  - `tests/host/shim/i2s_shim.c`
  - `tests/host/test_michi_audio_output.c`
- Invariants & Improvements:
  1. `B1 - Single I2S Owner`: ONLY `i2s_task()` owns `s_chunk`, modifies `s_chunk`, calls `michi_volume_apply()`, and calls `i2s_channel_write()`. Removed direct writes, silence pushes, and `memset(s_chunk, 0)` from caller contexts.
  2. `B2 - Control Command & ACK Protocol`: Implemented command dispatch (`QUIESCE`, `RESUME`, `STOP`) with `s_cmd_mux` serialization and `s_cmd_ack_sem` worker acknowledgement.
  3. `B3 - Quiesce Semantics`: Worker stops accepting stale PCM, flushes SPSC ring, clears `s_chunk`, pushes digital silence to clear hardware FIFOs, transitions to `MICHI_AUDIO_STATE_QUIESCED`, and signals ACK. New writes rejected with `ESP_ERR_INVALID_STATE`.
  4. `B4 - Resume Semantics`: Resets state from `QUIESCED` to `RUNNING`, signals ACK, fresh PCM accepted, old PCM never reappears.
  5. `B5 - I2S Timeout Units`: Verified against ESP-IDF headers (`timeout_ms`). Removed all `pdMS_TO_TICKS()` conversions on `i2s_channel_write` calls; using explicit millisecond constants `MICHI_AUDIO_I2S_WRITE_TIMEOUT_MS = 100` and `MICHI_AUDIO_I2S_SILENCE_TIMEOUT_MS = 50`.
  6. `B6 - Error Propagation`: Worker captures `i2s_channel_write` errors during quiesce, transitions to `MICHI_AUDIO_STATE_FAULTED`, and propagates error to caller without returning false `ESP_OK`.
  7. `B7 - State Machine`: Explicit state machine `michi_audio_output_state_t` (`UNINITIALIZED`, `INITIALIZED`, `RUNNING`, `QUIESCED`, `STOPPING`, `STOPPED`, `FAULTED`) with `michi_audio_output_get_state()`.
  8. `B8 - Deterministic Tests Added`: `AUDIO-Q-01` through `AUDIO-Q-10` in `test_michi_audio_output.c` (10/10 PASS).
## Wave C Evidence (Remaining Software Blockers: C1..C13)
- Scope Hardened:
  - `firmware/components/michi_board/waveshare_s3_lcd2/board_waveshare_s3_lcd2.c`
  - `firmware/components/michi_http/include/michi_http.h`
  - `firmware/components/michi_http/http_server.c`
  - `firmware/components/michi_http/json_helpers.c`
  - `firmware/components/michi_session/include/michi_session.h`
  - `firmware/components/michi_session/michi_session.c`
  - `firmware/components/michi_product_profile/capabilities.c`
  - `firmware/components/michi_time/Kconfig`
  - `firmware/sdkconfig.defaults`
  - `.github/workflows/ci.yml`
  - `tests/host/Makefile`
  - `tests/host/shim/esp_http_server.h`
  - `tests/host/shim/esp_http_server_shim.c`
  - `tests/host/shim/esp_lcd_shim.c`
  - `tests/host/shim/sdkconfig.h`
  - `tests/host/test_michi_display_dma.c`
  - `tests/host/test_session_http.c`
  - `tests/host/test_michi_session.c`
- Invariants & Improvements:
  1. `C1 & C2 - Display DMA Shutdown Safety & Late-completion Tests`:
     - `michi_board_shutdown()` waits up to 1000ms for in-flight DMA on `s_trans_done_sem`.
     - On timeout, returns `ESP_ERR_TIMEOUT`, preserves resources (`s_trans_done_sem`, `s_panel_io`, `s_panel`, `s_fb`, `spi_bus`), sets `s_dma_quarantined = true`, preventing UAF.
     - Once DMA finishes, retry cleanly reclaims resources and completes teardown.
     - Deterministic tests `DMA-SHUT-01..04` added in `test_michi_display_dma.c`.
  2. `C3 & C4 - HTTP Body Timeout Contradiction & Slowloris`:
     - Harmonized timeouts: `MICHI_HTTP_RECV_WAIT_TIMEOUT_S = 1`, `MICHI_HTTP_RECV_TIMEOUT_RETRIES = 1`, `MICHI_HTTP_BODY_TOTAL_TIMEOUT_MS = 2000`.
     - Moved `michi_http_read_body()` to `json_helpers.c` ensuring 100% production source parity between firmware and host tests without divergence.
     - Deterministic tests `HTTP-SLOW-01..03` added in `test_session_http.c`.
  3. `C5 - Heartbeat Peer-IP Fail Closed`:
     - `michi_session_heartbeat()` strictly verifies `peer_ip`: NULL, empty, or mismatch rejects fail-closed with `MICHI_SESSION_HEARTBEAT_SOURCE_MISMATCH` (HTTP 403 Forbidden).
     - Does not advance sequence number, does not renew lease. Legitimate peer can still send subsequent sequence.
     - Verified in `test_michi_session.c`.
  4. `C6 - PCM5122 Capability Layers`:
     - Documented the 4 distinct capability tiers in `capabilities.c`: Silicon Hardware Limits, Driver Implementation, Validated System Capability, and Wire Protocol / Advertised Capability.
  5. `C7 - Cppcheck Representative Variants`:
     - Updated `.github/workflows/ci.yml` to check Variant A (`CONFIG_MICHI_DAC_DEFAULT_PROFILE=""`) and Variant B (`CONFIG_MICHI_DAC_DEFAULT_PROFILE="pcm5102a"`).
     - Accurate suppression commentary for system includes and branch limits. Zero codebase warnings.
  6. `C8 - Host Kconfig Truth`:
     - `tests/host/shim/sdkconfig.h` classified into `TEST_OVERRIDE` (fast pairing window 5s, fast SNTP timeout 250ms, compact ring 64KB) and `PRODUCTION_DEFAULT`.
  7. `C9 - Stale Kconfig Removal`:
     - Removed obsolete `CONFIG_LWIP_SNTP=y` from `firmware/sdkconfig.defaults` and updated commentary in `firmware/components/michi_time/Kconfig` to reference `esp_netif_sntp` authority.
  8. `C10 - Signal Truth & Comments`:
     - Harmonized comments across `firmware/README.md`, `michi_http.h`, and component headers.
  9. `C11, C12, C13 - Regression Verifications`:
     - Boundary and nominal `buffer_ms` values (50, 300, 500) verified in `test_michi_session.c`.
     - Contract schemas and cases pass 100% (13/13).
     - RTP clock and jitter tests pass 100%.
- Verification:
## Wave D Evidence (Pre-Merge Certification & Full CI: D1..D7)
- Scope Hardened:
  - `tests/e2e/run_e2e.py`
  - `tests/e2e/results/michi-link-alpha1.json`
  - `.opencode/michi_stream_audit_remediation_state.md`
- Invariants & Improvements:
  1. `D1 - Full E2E Execution`:
     - Canonical receiver simulator and contract test suites execute all 13 cases (`E2E-01..E2E-13`) and 9 pytest modules cleanly.
  2. `D2 - E2E Re-Anchor`:
     - `STREAM_TESTED_COMMIT` re-anchored to `7a8733dc4c5204ae79a4ea7db9e9b448f5288064` (Wave C HEAD, verifying 0 drift across `firmware/`, `simulator/`, and `contracts/`).
     - Deterministic certification results artifact `tests/e2e/results/michi-link-alpha1.json` regenerated and synchronized.
  3. `D3 - Vendored Contract Sync Check`:
     - `python3 scripts/sync_michi_link_contract.py --check && python3 scripts/sync_michi_link_contract.py --verify-source` verified 100% byte-identical against `michi-link-v1.0.0-alpha.1` (84b72029e00d).
  4. `D4 - Full Host Test Suite`:
     - Clean host test run: `make -C tests/host clean && make -C tests/host test` passes 100% across all unit and integration suites.
  5. `D5 - Static Analysis / Cppcheck`:
     - Cppcheck passes with 0 warnings across both Variant A (`CONFIG_MICHI_DAC_DEFAULT_PROFILE=""`) and Variant B (`CONFIG_MICHI_DAC_DEFAULT_PROFILE="pcm5102a"`).
  6. `D6 - ESP-IDF Firmware Build`:
     - ESP-IDF release-v5.3 docker build passes cleanly (`michi-music-stream.bin` size: 1628896 bytes <= 4194304 bytes, SPIRAM OCT 16MB).
  7. `D7 - Pre-Merge Software Closure Candidate`:
     - All software blockers across Waves A, B, C, and D closed.
     - Note: Hardware-dependent verification gates (`DEVICE_E2E_PASS`, `HARDWARE_AUDIO_PASS`) remain honestly `PENDING` physical target flashing.
- Verification:
  - E2E certification: 13/13 PASS (`MOCK_PASS: true`).
  - Host test suite: 100% PASS.
  - Cppcheck: 0 warnings, 0 errors.
  - ESP-IDF release-v5.3 docker firmware build: PASS.





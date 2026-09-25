# Michi Stream KILLCRITIC — Round 3

START_HEAD: a44ea3c803a2bd62cfac772cd9d1463f80523c5c
CURRENT_HEAD: 824815e61bf969543e33e9d8e578c772e2cf631b
BRANCH: fix/ui-device-gaps
WORKTREE_STATUS: clean

GLOBAL_STATUS: IN_PROGRESS

| Phase | Status | Reproduced | Test before patch | Patch | Falsified | Firmware | Commit |
|---|---|---|---|---|---|---|---|
| R3-00 | PASS | YES | YES | N/A | YES | FAIL (reproduced) | - |
| R3-01 | PASS | YES | YES | YES | YES | PASS | c653914 |
| R3-02 | PASS | YES | YES | YES | YES | PASS | 3276953 |
| R3-03 | PASS | YES | YES | YES | YES | PASS | 824815e |
| R3-04 | TODO | NO | NO | NO | NO | NO | - |
| R3-05 | TODO | NO | NO | NO | NO | NO | - |
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


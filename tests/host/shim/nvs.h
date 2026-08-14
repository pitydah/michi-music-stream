#pragma once
/* Shim for host-side tests: minimal fake NVS in RAM (test double, not a
 * reimplementation of flash NVS). Lets the REAL identity_nvs.c run
 * unchanged on the host. TEST-ONLY: never compiled into firmware.
 *
 * Implementation lives in nvs_fake.c (a single shared store across all
 * translation units, like a real NVS partition).
 *
 * Behavior mirrors the ESP-IDF contract the identity wrapper relies on:
 *  - nvs_open(READONLY) on an unknown namespace -> ESP_ERR_NVS_NOT_FOUND;
 *  - nvs_get_blob on a missing key -> ESP_ERR_NVS_NOT_FOUND;
 *  - nvs_open on a known namespace -> ESP_OK with a handle.
 *
 * Test hooks: test_nvs_reset(), test_nvs_write_count(),
 * test_nvs_force_read_error(), test_nvs_get_blob() (raw store inspection),
 * test_nvs_store() (raw namespace dump). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_READONLY 0x01
#define NVS_READWRITE 0x02

typedef int32_t nvs_handle_t;

#define TEST_NVS_MAX_NAMESPACES 8
#define TEST_NVS_MAX_BLOBS 4096

typedef struct {
    char key[16];
    uint8_t data[TEST_NVS_MAX_BLOBS];
    size_t len;
    bool present;
} test_nvs_entry_t;

typedef struct {
    char name[16];
    bool present;
    test_nvs_entry_t entries[TEST_NVS_MAX_NAMESPACES];
    uint32_t write_count;
    bool force_read_error;
} test_nvs_namespace_t;

/* --- test hooks (raw store inspection) --- */

test_nvs_namespace_t *test_nvs_store(void);
void test_nvs_reset(void);
uint32_t test_nvs_write_count(const char *ns);
void test_nvs_force_read_error(const char *ns, bool force);
bool test_nvs_get_blob(const char *ns, const char *key, uint8_t *out,
                       size_t out_cap, size_t *out_len);

/* --- fake NVS API (used by the firmware sources under test) --- */

esp_err_t nvs_open(const char *ns_name, int mode, nvs_handle_t *out_handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out,
                       size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
void nvs_close(nvs_handle_t handle);

#ifdef __cplusplus
}
#endif

/* Fake NVS implementation for host-side tests (see nvs.h). One shared
 * in-RAM store across all translation units. TEST-ONLY. */

#include "nvs.h"

#include <string.h>

test_nvs_namespace_t *test_nvs_store(void)
{
    static test_nvs_namespace_t s_store[TEST_NVS_MAX_NAMESPACES];
    static bool s_inited = false;
    if (!s_inited) {
        memset(s_store, 0, sizeof(s_store));
        s_inited = true;
    }
    return s_store;
}

void test_nvs_reset(void)
{
    memset(test_nvs_store(), 0,
           TEST_NVS_MAX_NAMESPACES * sizeof(test_nvs_namespace_t));
}

uint32_t test_nvs_write_count(const char *ns)
{
    test_nvs_namespace_t *store = test_nvs_store();
    for (int i = 0; i < TEST_NVS_MAX_NAMESPACES; i++) {
        if (store[i].present && strcmp(store[i].name, ns) == 0) {
            return store[i].write_count;
        }
    }
    return 0;
}

void test_nvs_force_read_error(const char *ns, bool force)
{
    test_nvs_namespace_t *store = test_nvs_store();
    for (int i = 0; i < TEST_NVS_MAX_NAMESPACES; i++) {
        if (store[i].present && strcmp(store[i].name, ns) == 0) {
            store[i].force_read_error = force;
            return;
        }
    }
}

bool test_nvs_get_blob(const char *ns, const char *key, uint8_t *out,
                       size_t out_cap, size_t *out_len)
{
    test_nvs_namespace_t *store = test_nvs_store();
    for (int i = 0; i < TEST_NVS_MAX_NAMESPACES; i++) {
        if (!store[i].present || strcmp(store[i].name, ns) != 0) {
            continue;
        }
        for (int j = 0; j < TEST_NVS_MAX_NAMESPACES; j++) {
            test_nvs_entry_t *e = &store[i].entries[j];
            if (e->present && strcmp(e->key, key) == 0) {
                if (out != NULL && out_cap >= e->len) {
                    memcpy(out, e->data, e->len);
                }
                *out_len = e->len;
                return true;
            }
        }
    }
    return false;
}

esp_err_t nvs_open(const char *ns_name, int mode, nvs_handle_t *out_handle)
{
    if (out_handle == NULL || ns_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    test_nvs_namespace_t *store = test_nvs_store();
    for (int i = 0; i < TEST_NVS_MAX_NAMESPACES; i++) {
        if (store[i].present && strcmp(store[i].name, ns_name) == 0) {
            *out_handle = i;
            return ESP_OK;
        }
    }
    /* Unknown namespace: NOT_FOUND for read-only opens, auto-create for
     * write opens (mirrors NVS behavior). */
    if (mode == NVS_READONLY) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    for (int i = 0; i < TEST_NVS_MAX_NAMESPACES; i++) {
        if (!store[i].present) {
            store[i].present = true;
            strncpy(store[i].name, ns_name, sizeof(store[i].name) - 1);
            *out_handle = i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out,
                       size_t *length)
{
    test_nvs_namespace_t *store = test_nvs_store();
    if (handle < 0 || handle >= TEST_NVS_MAX_NAMESPACES ||
        !store[handle].present || key == NULL || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (store[handle].force_read_error) {
        return ESP_FAIL; /* simulated flash/CRC read failure */
    }
    for (int j = 0; j < TEST_NVS_MAX_NAMESPACES; j++) {
        test_nvs_entry_t *e = &store[handle].entries[j];
        if (e->present && strcmp(e->key, key) == 0) {
            if (out != NULL) {
                memcpy(out, e->data, e->len < *length ? e->len : *length);
            }
            *length = e->len;
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length)
{
    test_nvs_namespace_t *store = test_nvs_store();
    if (handle < 0 || handle >= TEST_NVS_MAX_NAMESPACES ||
        !store[handle].present || key == NULL || value == NULL ||
        length > TEST_NVS_MAX_BLOBS) {
        return ESP_ERR_INVALID_ARG;
    }
    test_nvs_entry_t *e = NULL;
    for (int j = 0; j < TEST_NVS_MAX_NAMESPACES; j++) {
        if (store[handle].entries[j].present &&
            strcmp(store[handle].entries[j].key, key) == 0) {
            e = &store[handle].entries[j];
            break;
        }
        if (e == NULL && !store[handle].entries[j].present) {
            e = &store[handle].entries[j];
        }
    }
    if (e == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strncpy(e->key, key, sizeof(e->key) - 1);
    memcpy(e->data, value, length);
    e->len = length;
    e->present = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    test_nvs_namespace_t *store = test_nvs_store();
    if (handle < 0 || handle >= TEST_NVS_MAX_NAMESPACES ||
        !store[handle].present) {
        return ESP_ERR_INVALID_ARG;
    }
    store[handle].write_count++;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    test_nvs_namespace_t *store = test_nvs_store();
    if (handle < 0 || handle >= TEST_NVS_MAX_NAMESPACES ||
        !store[handle].present) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int j = 0; j < TEST_NVS_MAX_NAMESPACES; j++) {
        test_nvs_entry_t *e = &store[handle].entries[j];
        if (e->present && strcmp(e->key, key) == 0) {
            memset(e, 0, sizeof(*e));
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

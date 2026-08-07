/* JSON access helpers with exact-type semantics (F15: extracted from
 * http_server.c so the host-side tests compile the SAME source - no
 * reimplementation). Uses cJSON; on the host the tests link the system
 * libcjson (CI: apt install libcjson-dev). */

#include "michi_http.h"

#include <limits.h>
#include <string.h>

#include "cJSON.h"

bool michi_http_json_get_string(const cJSON *obj, const char *key,
                                char *out, size_t out_len)
{
    if (obj == NULL || key == NULL || out == NULL || out_len == 0) {
        return false;
    }
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item == NULL || !cJSON_IsString(item) ||
        item->valuestring == NULL) {
        return false;
    }
    size_t len = strlen(item->valuestring);
    if (len >= out_len) {
        return false; /* value does not fit: fail, do not truncate */
    }
    memcpy(out, item->valuestring, len + 1);
    return true;
}

bool michi_http_json_get_int(const cJSON *obj, const char *key, int *out)
{
    if (obj == NULL || key == NULL || out == NULL) {
        return false;
    }
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item == NULL || !cJSON_IsNumber(item)) {
        return false; /* exact type: no strings, no bools, no coercion */
    }
    /* Exact type PLUS range: fractional or out-of-int-range numbers fail
     * (never truncated); the (int) cast is safe after the range checks. */
    const double d = item->valuedouble;
    if (d < (double)INT_MIN || d > (double)INT_MAX ||
        d != (double)(int)d) {
        return false;
    }
    *out = item->valueint;
    return true;
}

bool michi_http_json_get_bool(const cJSON *obj, const char *key, bool *out)
{
    if (obj == NULL || key == NULL || out == NULL) {
        return false;
    }
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item == NULL || !cJSON_IsBool(item)) {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

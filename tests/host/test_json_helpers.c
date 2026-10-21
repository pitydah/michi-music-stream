/* Host-side tests for the checked JSON helpers (F15).
 * Compiles the REAL firmware source: components/michi_http/json_helpers.c
 * (linked from the Makefile) against the SYSTEM cJSON (CI:
 * apt install libcjson-dev) - no reimplementation. */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "michi_http.h"

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static cJSON *parse(const char *json)
{
    return cJSON_Parse(json);
}

/* ── get_string ───────────────────────────────────────────── */

static void test_get_string(void)
{
    printf("json: get_string\n");
    cJSON *obj = parse("{\"name\":\"hello\",\"num\":42,\"big\":\"toolong\"}");
    char buf[16];

    CHECK(michi_http_json_get_string(obj, "name", buf, sizeof(buf)),
          "string value");
    CHECK(strcmp(buf, "hello") == 0, "string content");

    CHECK(!michi_http_json_get_string(obj, "num", buf, sizeof(buf)),
          "wrong type (number) rejected");
    CHECK(!michi_http_json_get_string(obj, "missing", buf, sizeof(buf)),
          "missing key rejected");
    CHECK(!michi_http_json_get_string(obj, "big", buf, 4),
          "oversize rejected (would truncate)");
    CHECK(strcmp(buf, "hello") == 0, "oversize does NOT touch the buffer");
    CHECK(!michi_http_json_get_string(NULL, "name", buf, sizeof(buf)),
          "NULL obj rejected");
    CHECK(!michi_http_json_get_string(obj, NULL, buf, sizeof(buf)),
          "NULL key rejected");
    CHECK(!michi_http_json_get_string(obj, "name", NULL, sizeof(buf)),
          "NULL out rejected");
    CHECK(!michi_http_json_get_string(obj, "name", buf, 0),
          "zero size rejected");

    cJSON_Delete(obj);
}

/* ── get_int ──────────────────────────────────────────────── */

static void test_get_int(void)
{
    printf("json: get_int\n");
    cJSON *obj = parse("{\"i\":42,\"f\":3.5,\"big\":2147483648,"
                       "\"neg\":-2147483648,\"s\":\"42\",\"b\":true}");
    int v = 0;

    CHECK(michi_http_json_get_int(obj, "i", &v), "exact integer");
    CHECK(v == 42, "integer value");

    CHECK(!michi_http_json_get_int(obj, "f", &v), "fraction rejected");
    CHECK(!michi_http_json_get_int(obj, "big", &v), "above INT_MAX rejected");
    CHECK(!michi_http_json_get_int(obj, "s", &v), "string rejected (no coercion)");
    CHECK(!michi_http_json_get_int(obj, "b", &v), "bool rejected (no coercion)");
    CHECK(!michi_http_json_get_int(obj, "missing", &v), "missing key rejected");
    CHECK(!michi_http_json_get_int(NULL, "i", &v), "NULL obj rejected");
    CHECK(!michi_http_json_get_int(obj, "i", NULL), "NULL out rejected");

    /* Exact INT_MIN boundary must round-trip. */
    CHECK(michi_http_json_get_int(obj, "neg", &v), "INT_MIN accepted");
    CHECK(v == -2147483647 - 1, "INT_MIN value");

    /* 1.0 and 2.0 are integral doubles: must be accepted. */
    cJSON_Delete(obj);
    obj = parse("{\"x\":1.0,\"y\":2.0}");
    CHECK(michi_http_json_get_int(obj, "x", &v) && v == 1, "1.0 accepted");
    CHECK(michi_http_json_get_int(obj, "y", &v) && v == 2, "2.0 accepted");

    cJSON_Delete(obj);
}

/* ── get_bool ─────────────────────────────────────────────── */

static void test_get_bool(void)
{
    printf("json: get_bool\n");
    cJSON *obj = parse("{\"t\":true,\"f\":false,\"n\":1,\"s\":\"true\"}");
    bool v = false;

    CHECK(michi_http_json_get_bool(obj, "t", &v), "true");
    CHECK(v == true, "true value");
    CHECK(michi_http_json_get_bool(obj, "f", &v), "false");
    CHECK(v == false, "false value");

    CHECK(!michi_http_json_get_bool(obj, "n", &v), "number rejected (strict)");
    CHECK(!michi_http_json_get_bool(obj, "s", &v), "string rejected (strict)");
    CHECK(!michi_http_json_get_bool(obj, "missing", &v), "missing key rejected");
    CHECK(!michi_http_json_get_bool(NULL, "t", &v), "NULL obj rejected");
    CHECK(!michi_http_json_get_bool(obj, "t", NULL), "NULL out rejected");

    cJSON_Delete(obj);
}

/* The live HTTP request parser must reject ambiguous wire encodings. */
static void test_strict_wire_parser(void)
{
    printf("json: strict complete body\n");
    const char *good = "{\"source\":\"Sala\",\"revision\":2}";
    cJSON *obj = michi_http_json_parse_strict(good, strlen(good));
    CHECK(obj != NULL, "valid object accepted");
    cJSON_Delete(obj);

    const char *trailing = "{\"ok\":true} garbage";
    CHECK(michi_http_json_parse_strict(trailing, strlen(trailing)) == NULL,
          "trailing payload rejected");
    const char *array = "[1,2,3]";
    CHECK(michi_http_json_parse_strict(array, strlen(array)) == NULL,
          "non-object rejected");
    const char *escaped_nul = "{\"x\":\"\\u0000\"}";
    CHECK(michi_http_json_parse_strict(escaped_nul, strlen(escaped_nul)) == NULL,
          "Unicode NUL escape rejected");
    const char *escaped_literal = "{\"x\":\"\\\\u0000\"}";
    obj = michi_http_json_parse_strict(escaped_literal, strlen(escaped_literal));
    CHECK(obj != NULL, "literal backslash-u is not a NUL escape");
    cJSON_Delete(obj);
    const char embedded[] = {'{','}','\0','X','\0'};
    CHECK(michi_http_json_parse_strict(embedded, 4) == NULL,
          "embedded wire NUL rejected");
    CHECK(michi_http_json_parse_strict(NULL, 0) == NULL,
          "NULL body rejected");
}

static void test_utf8_metadata(void)
{
    printf("json: UTF-8 metadata\n");
    const char *spanish = "Música española";
    CHECK(michi_http_utf8_text_valid(spanish, strlen(spanish)),
          "valid multilingual title accepted");
    CHECK(michi_http_utf8_text_valid("", 0), "empty metadata accepted");
    const char *truncated = "\xc3";
    CHECK(!michi_http_utf8_text_valid(truncated, 1),
          "truncated sequence rejected");
    const char *overlong = "\xc0\xaf";
    CHECK(!michi_http_utf8_text_valid(overlong, 2),
          "overlong UTF-8 rejected");
    const char *surrogate = "\xed\xa0\x80";
    CHECK(!michi_http_utf8_text_valid(surrogate, 3),
          "UTF-8 surrogate rejected");
    const char *above = "\xf4\x90\x80\x80";
    CHECK(!michi_http_utf8_text_valid(above, 4),
          "non-Unicode scalar rejected");
    const char *control = "line\nfeed";
    CHECK(!michi_http_utf8_text_valid(control, strlen(control)),
          "embedded control rejected");
    CHECK(!michi_http_utf8_text_valid(NULL, 0), "NULL text rejected");
}

int main(void)
{
    test_get_string();
    test_get_int();
    test_get_bool();
    test_strict_wire_parser();
    test_utf8_metadata();
    if (failures == 0) {
        printf("PASS test_json_helpers\n");
        return 0;
    }
    printf("FAIL test_json_helpers (%d)\n", failures);
    return 1;
}

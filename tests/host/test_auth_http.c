/* Host-side tests for Home Membership HTTP body gates.
 *
 * Compiles the REAL firmware sources:
 *   - components/michi_http/json_helpers.c
 *   - components/michi_home/validators.c
 * against the SYSTEM cJSON (CI: libcjson-dev).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "michi_http.h"
#include "validators.h"
#include "michi_home.h"
#include "michi_auth.h"

static int failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL %s (line %d)\n", msg, __LINE__); \
            failures++; \
        } \
    } while (0)

static char err_field[32];

static const char VALID_CHALLENGE_JSON[] =
    "{\"client_michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
    "\"client_public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E\","
    "\"home_id\":\"FU1FL-wFLfsfew3qpbR7XjDkmStWZY4g84MyW-zXPOs\"}";

static const char VALID_SESSION_JSON[] =
    "{\"challenge_id\":\"550e8400-e29b-41d4-a716-446655440001\","
    "\"client_michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
    "\"membership\":{"
    "\"version\":1,"
    "\"home_id\":\"FU1FL-wFLfsfew3qpbR7XjDkmStWZY4g84MyW-zXPOs\","
    "\"device_michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
    "\"device_public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E\","
    "\"device_type\":\"server\","
    "\"roles\":[\"music_server\"],"
    "\"issued_at\":\"2026-10-04T12:00:00Z\","
    "\"serial\":1,"
    "\"signature\":\"5Hg1TwCbzj-x6MaU7mvToRCALEyQXtRKmeJWLmShuXuJWSes16wGptbq573FfY3_H5VWRxPU9LI8hanyNfbXCA\""
    "},"
    "\"client_signature\":\"5Hg1TwCbzj-x6MaU7mvToRCALEyQXtRKmeJWLmShuXuJWSes16wGptbq573FfY3_H5VWRxPU9LI8hanyNfbXCA\"}";

static void test_auth_challenge_parsing(void)
{
    printf("auth http: POST /api/v1/auth/challenge body parsing\n");
    cJSON *root = cJSON_Parse(VALID_CHALLENGE_JSON);
    CHECK(root != NULL, "json parses");

    char client_michi_id[44] = {0};
    char client_pk[44] = {0};
    char home_id[44] = {0};

    bool ok = michi_http_json_get_auth_challenge(
        root, client_michi_id, sizeof(client_michi_id),
        client_pk, sizeof(client_pk),
        home_id, sizeof(home_id),
        err_field, sizeof(err_field));
    CHECK(ok, "valid challenge json accepted");
    CHECK(strcmp(client_michi_id, "JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U") == 0, "client_michi_id matches");
    CHECK(strcmp(client_pk, "j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E") == 0, "client_pk matches");
    CHECK(strcmp(home_id, "FU1FL-wFLfsfew3qpbR7XjDkmStWZY4g84MyW-zXPOs") == 0, "home_id matches");
    cJSON_Delete(root);

    /* Malformed client_michi_id length */
    const char *bad_id =
        "{\"client_michi_id\":\"tooshort\","
        "\"client_public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E\","
        "\"home_id\":\"FU1FL-wFLfsfew3qpbR7XjDkmStWZY4g84MyW-zXPOs\"}";
    root = cJSON_Parse(bad_id);
    ok = michi_http_json_get_auth_challenge(
        root, client_michi_id, sizeof(client_michi_id),
        client_pk, sizeof(client_pk),
        home_id, sizeof(home_id),
        err_field, sizeof(err_field));
    CHECK(!ok, "short client_michi_id rejected");
    CHECK(strcmp(err_field, "client_michi_id") == 0, "err_field set to client_michi_id");
    cJSON_Delete(root);
}

static void test_auth_session_parsing(void)
{
    printf("auth http: POST /api/v1/auth/session body parsing\n");
    cJSON *root = cJSON_Parse(VALID_SESSION_JSON);
    CHECK(root != NULL, "json parses");

    char challenge_id[MICHI_AUTH_CHALLENGE_ID_LEN] = {0};
    char client_michi_id[44] = {0};
    michi_membership_t mem;
    char client_sig[MICHI_HOME_SIG_B64_LEN] = {0};

    bool ok = michi_http_json_get_auth_session(
        root, challenge_id, sizeof(challenge_id),
        client_michi_id, sizeof(client_michi_id),
        &mem,
        client_sig, sizeof(client_sig),
        err_field, sizeof(err_field));
    CHECK(ok, "valid session json accepted");
    CHECK(strcmp(challenge_id, "550e8400-e29b-41d4-a716-446655440001") == 0, "challenge_id matches");
    CHECK(mem.version == 1, "version is 1");
    CHECK(strcmp(mem.home_id, "FU1FL-wFLfsfew3qpbR7XjDkmStWZY4g84MyW-zXPOs") == 0, "membership home_id matches");
    CHECK(strcmp(mem.roles[0], "music_server") == 0, "role matches");
    CHECK(mem.serial == 1, "serial matches");
    cJSON_Delete(root);

    /* Malformed challenge_id (not a UUID) */
    const char *bad_cid =
        "{\"challenge_id\":\"not-a-valid-uuid\","
        "\"client_michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
        "\"membership\":{\"version\":1},"
        "\"client_signature\":\"5Hg1TwCbzj-x6MaU7mvToRCALEyQXtRKmeJWLmShuXuJWSes16wGptbq573FfY3_H5VWRxPU9LI8hanyNfbXCA\"}";
    root = cJSON_Parse(bad_cid);
    ok = michi_http_json_get_auth_session(
        root, challenge_id, sizeof(challenge_id),
        client_michi_id, sizeof(client_michi_id),
        &mem,
        client_sig, sizeof(client_sig),
        err_field, sizeof(err_field));
    CHECK(!ok, "invalid challenge_id rejected");
    CHECK(strcmp(err_field, "challenge_id") == 0, "err_field set to challenge_id");
    cJSON_Delete(root);
}

int main(void)
{
    test_auth_challenge_parsing();
    test_auth_session_parsing();

    if (failures == 0) {
        printf("PASS test_auth_http\n");
        return 0;
    }
    printf("FAIL test_auth_http (%d)\n", failures);
    return 1;
}

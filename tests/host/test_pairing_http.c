/* Host-side tests for the pairing HTTP body gates (MS-06).
 *
 * Compiles the REAL firmware source: components/michi_http/json_helpers.c
 * + components/michi_pairing/validators.c (linked from the Makefile)
 * against the SYSTEM cJSON (CI: apt install libcjson-dev) - no
 * reimplementation.
 *
 * Covers:
 *  - the canonical pair/start and pair/confirm body copies (golden
 *    vectors from contracts/michi-link/vectors/pairing/);
 *  - the EXPLICIT rejection of the legacy fields initiator_id and
 *    client_token (400; the acceptance rg depends on these being the
 *    only live references outside the legacy e2e/common trees);
 *  - malformed/missing field rejections with the offending field name.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "michi_http.h"
#include "validators.h"

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static const char VEC_PAIR_START[] =
    "{\"device_name\":\"Michi Micro Server\",\"device_type\":\"server\","
    "\"roles\":[\"music_server\"],\"auth_strategy\":\"RECEIVER_BUTTON\","
    "\"michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
    "\"public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E\","
    "\"challenge_nonce\":\"CxIZICcuNTxDSlFYX2ZtdA\","
    "\"challenge_signature\":\"5Hg1TwCbzj-x6MaU7mvToRCALEyQXtRKmeJWLmShuXuJWSes16wGptbq573FfY3_H5VWRxPU9LI8hanyNfbXCA\"}";

static const char VEC_PAIR_CONFIRM[] =
    "{\"session_id\":\"550e8400-e29b-41d4-a716-446655440001\","
    "\"pin\":\"042731\","
    "\"michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
    "\"public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E\"}";

static char err_field[32];

static void test_pair_start_valid(void)
{
    printf("pairing http: pair/start valid body\n");
    cJSON *root = cJSON_Parse(VEC_PAIR_START);
    CHECK(root != NULL, "vector parses");
    char mi[44], pk[44], nonce[64], sig[87];
    const bool ok = michi_http_json_get_pair_start(
        root, mi, sizeof(mi), pk, sizeof(pk), nonce, sizeof(nonce), sig,
        sizeof(sig), err_field, sizeof(err_field));
    CHECK(ok, "pair/start vector accepted");
    CHECK(strcmp(mi, "JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U") == 0,
          "michi_id copied");
    CHECK(strcmp(pk, "j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E") == 0,
          "public_key copied");
    CHECK(strcmp(nonce, "CxIZICcuNTxDSlFYX2ZtdA") == 0, "nonce copied");
    CHECK(strcmp(sig,
                 "5Hg1TwCbzj-x6MaU7mvToRCALEyQXtRKmeJWLmShuXuJWSes16wGptbq573FfY3_H5VWRxPU9LI8hanyNfbXCA") == 0,
          "signature copied");
    cJSON_Delete(root);
}

static void test_pair_start_rejects_legacy(void)
{
    printf("pairing http: pair/start rejects initiator_id/client_token\n");
    const char *with_initiator =
        "{\"device_name\":\"Michi Micro Server\",\"device_type\":\"server\","
        "\"roles\":[\"music_server\"],\"auth_strategy\":\"RECEIVER_BUTTON\","
        "\"initiator_id\":\"m1\","
        "\"michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
        "\"public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E\","
        "\"challenge_nonce\":\"CxIZICcuNTxDSlFYX2ZtdA\","
        "\"challenge_signature\":\"5Hg1TwCbzj-x6MaU7mvToRCALEyQXtRKmeJWLmShuXuJWSes16wGptbq573FfY3_H5VWRxPU9LI8hanyNfbXCA\"}";
    cJSON *root = cJSON_Parse(with_initiator);
    CHECK(root != NULL, "initiator vector parses");
    char mi[44], pk[44], nonce[64], sig[87];
    CHECK(!michi_http_json_get_pair_start(root, mi, sizeof(mi), pk,
                                          sizeof(pk), nonce, sizeof(nonce),
                                          sig, sizeof(sig), err_field,
                                          sizeof(err_field)),
          "initiator_id rejected");
    CHECK(strcmp(err_field, "initiator_id") == 0, "field names initiator_id");
    cJSON_Delete(root);

    const char *with_client_token =
        "{\"device_name\":\"Michi Micro Server\",\"device_type\":\"server\","
        "\"roles\":[\"music_server\"],\"auth_strategy\":\"RECEIVER_BUTTON\","
        "\"client_token\":\"abc\","
        "\"michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
        "\"public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E\","
        "\"challenge_nonce\":\"CxIZICcuNTxDSlFYX2ZtdA\","
        "\"challenge_signature\":\"5Hg1TwCbzj-x6MaU7mvToRCALEyQXtRKmeJWLmShuXuJWSes16wGptbq573FfY3_H5VWRxPU9LI8hanyNfbXCA\"}";
    root = cJSON_Parse(with_client_token);
    CHECK(root != NULL, "client_token vector parses");
    CHECK(!michi_http_json_get_pair_start(root, mi, sizeof(mi), pk,
                                          sizeof(pk), nonce, sizeof(nonce),
                                          sig, sizeof(sig), err_field,
                                          sizeof(err_field)),
          "client_token rejected");
    CHECK(strcmp(err_field, "client_token") == 0, "field names client_token");
    cJSON_Delete(root);
}

static void test_pair_start_malformed(void)
{
    printf("pairing http: pair/start malformed bodies\n");
    char mi[44], pk[44], nonce[64], sig[87];

    /* Missing signature. */
    const char *no_sig =
        "{\"device_name\":\"Michi Micro Server\",\"device_type\":\"server\","
        "\"roles\":[\"music_server\"],\"auth_strategy\":\"RECEIVER_BUTTON\","
        "\"michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
        "\"public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E\","
        "\"challenge_nonce\":\"CxIZICcuNTxDSlFYX2ZtdA\"}";
    cJSON *root = cJSON_Parse(no_sig);
    CHECK(root != NULL, "no_sig parses");
    CHECK(!michi_http_json_get_pair_start(root, mi, sizeof(mi), pk,
                                          sizeof(pk), nonce, sizeof(nonce),
                                          sig, sizeof(sig), err_field,
                                          sizeof(err_field)),
          "missing signature rejected");
    CHECK(strcmp(err_field, "challenge_signature") == 0, "field named");
    cJSON_Delete(root);

    /* Wrong-length michi_id (42 chars). */
    const char *bad_id =
        "{\"device_name\":\"Michi Micro Server\",\"device_type\":\"server\","
        "\"roles\":[\"music_server\"],\"auth_strategy\":\"RECEIVER_BUTTON\","
        "\"michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-\","
        "\"public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E\","
        "\"challenge_nonce\":\"CxIZICcuNTxDSlFYX2ZtdA\","
        "\"challenge_signature\":\"5Hg1TwCbzj-x6MaU7mvToRCALEyQXtRKmeJWLmShuXuJWSes16wGptbq573FfY3_H5VWRxPU9LI8hanyNfbXCA\"}";
    root = cJSON_Parse(bad_id);
    CHECK(root != NULL, "bad_id parses");
    CHECK(!michi_http_json_get_pair_start(root, mi, sizeof(mi), pk,
                                          sizeof(pk), nonce, sizeof(nonce),
                                          sig, sizeof(sig), err_field,
                                          sizeof(err_field)),
          "42-char michi_id rejected");
    CHECK(strcmp(err_field, "michi_id") == 0, "field named");
    cJSON_Delete(root);

    /* Short nonce (21 chars; schema minimum is 22). */
    const char *short_nonce =
        "{\"device_name\":\"Michi Micro Server\",\"device_type\":\"server\","
        "\"roles\":[\"music_server\"],\"auth_strategy\":\"RECEIVER_BUTTON\","
        "\"michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
        "\"public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E\","
        "\"challenge_nonce\":\"CxIZICcuNTxDSlFYX2Zt\","
        "\"challenge_signature\":\"5Hg1TwCbzj-x6MaU7mvToRCALEyQXtRKmeJWLmShuXuJWSes16wGptbq573FfY3_H5VWRxPU9LI8hanyNfbXCA\"}";
    root = cJSON_Parse(short_nonce);
    CHECK(root != NULL, "short_nonce parses");
    CHECK(!michi_http_json_get_pair_start(root, mi, sizeof(mi), pk,
                                          sizeof(pk), nonce, sizeof(nonce),
                                          sig, sizeof(sig), err_field,
                                          sizeof(err_field)),
          "21-char nonce rejected");
    CHECK(strcmp(err_field, "challenge_nonce") == 0, "field named");
    cJSON_Delete(root);

    /* Non-b64url public key ('=' padding is rejected by the schema). */
    const char *padded_pk =
        "{\"device_name\":\"Michi Micro Server\",\"device_type\":\"server\","
        "\"roles\":[\"music_server\"],\"auth_strategy\":\"RECEIVER_BUTTON\","
        "\"michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
        "\"public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7=\","
        "\"challenge_nonce\":\"CxIZICcuNTxDSlFYX2ZtdA\","
        "\"challenge_signature\":\"5Hg1TwCbzj-x6MaU7mvToRCALEyQXtRKmeJWLmShuXuJWSes16wGptbq573FfY3_H5VWRxPU9LI8hanyNfbXCA\"}";
    root = cJSON_Parse(padded_pk);
    CHECK(root != NULL, "padded_pk parses");
    CHECK(!michi_http_json_get_pair_start(root, mi, sizeof(mi), pk,
                                          sizeof(pk), nonce, sizeof(nonce),
                                          sig, sizeof(sig), err_field,
                                          sizeof(err_field)),
          "padded public key rejected");
    CHECK(strcmp(err_field, "public_key") == 0, "field named");
    cJSON_Delete(root);
}

static void test_pair_confirm_valid(void)
{
    printf("pairing http: pair/confirm valid body\n");
    cJSON *root = cJSON_Parse(VEC_PAIR_CONFIRM);
    CHECK(root != NULL, "vector parses");
    char sid[37], pin[7], mi[44], pk[44];
    const bool ok = michi_http_json_get_pair_confirm(
        root, sid, sizeof(sid), pin, sizeof(pin), mi, sizeof(mi), pk,
        sizeof(pk), err_field, sizeof(err_field));
    CHECK(ok, "pair/confirm vector accepted");
    CHECK(strcmp(sid, "550e8400-e29b-41d4-a716-446655440001") == 0,
          "session_id copied");
    CHECK(strcmp(pin, "042731") == 0, "pin copied");
    CHECK(strcmp(mi, "JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U") == 0,
          "michi_id copied");
    CHECK(strcmp(pk, "j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E") == 0,
          "public_key copied");
    cJSON_Delete(root);
}

static void test_pair_confirm_rejects_legacy_and_malformed(void)
{
    printf("pairing http: pair/confirm legacy + malformed bodies\n");
    char sid[37], pin[7], mi[44], pk[44];

    const char *with_initiator =
        "{\"session_id\":\"550e8400-e29b-41d4-a716-446655440001\","
        "\"pin\":\"042731\",\"initiator_id\":\"m1\","
        "\"michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
        "\"public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E\"}";
    cJSON *root = cJSON_Parse(with_initiator);
    CHECK(root != NULL, "initiator vector parses");
    CHECK(!michi_http_json_get_pair_confirm(root, sid, sizeof(sid), pin,
                                            sizeof(pin), mi, sizeof(mi), pk,
                                            sizeof(pk), err_field,
                                            sizeof(err_field)),
          "initiator_id rejected");
    CHECK(strcmp(err_field, "initiator_id") == 0, "field names initiator_id");
    cJSON_Delete(root);

    const char *bad_pin =
        "{\"session_id\":\"550e8400-e29b-41d4-a716-446655440001\","
        "\"pin\":\"12345\","
        "\"michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
        "\"public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E\"}";
    root = cJSON_Parse(bad_pin);
    CHECK(root != NULL, "bad_pin parses");
    CHECK(!michi_http_json_get_pair_confirm(root, sid, sizeof(sid), pin,
                                            sizeof(pin), mi, sizeof(mi), pk,
                                            sizeof(pk), err_field,
                                            sizeof(err_field)),
          "5-digit pin rejected");
    CHECK(strcmp(err_field, "pin") == 0, "field named");
    cJSON_Delete(root);

    const char *bad_sid =
        "{\"session_id\":\"not-a-uuid\","
        "\"pin\":\"042731\","
        "\"michi_id\":\"JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U\","
        "\"public_key\":\"j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E\"}";
    root = cJSON_Parse(bad_sid);
    CHECK(root != NULL, "bad_sid parses");
    CHECK(!michi_http_json_get_pair_confirm(root, sid, sizeof(sid), pin,
                                            sizeof(pin), mi, sizeof(mi), pk,
                                            sizeof(pk), err_field,
                                            sizeof(err_field)),
          "malformed session_id rejected");
    CHECK(strcmp(err_field, "session_id") == 0, "field named");
    cJSON_Delete(root);
}

int main(void)
{
    test_pair_start_valid();
    test_pair_start_rejects_legacy();
    test_pair_start_malformed();
    test_pair_confirm_valid();
    test_pair_confirm_rejects_legacy_and_malformed();

    if (failures != 0) {
        printf("\n%d host pairing HTTP check(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall host pairing HTTP checks passed\n");
    return 0;
}

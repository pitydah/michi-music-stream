#include <string.h>
#include <cJSON.h>
#include "michi_link_lite.h"
#include "receiver_info.h"
#include "pairing.h"
#include "heartbeat.h"
#include "session.h"
#include "volume.h"
#if OTA_SUPPORTED
#include "ota_update.h"
#endif

static httpd_handle_t s_server = NULL;

static bool authenticate_request(httpd_req_t *req)
{
    char auth[256] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK)
        return false;
    if (strncmp(auth, "Bearer ", 7) != 0)
        return false;
    return pairing_validate_token(auth + 7);
}

static esp_err_t send_error(httpd_req_t *req, int status, const char *code, const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    cJSON_AddObjectToObject(e, "details");
    char *json = cJSON_PrintUnformatted(r);
    httpd_resp_set_status(req,
        status == 401 ? "401 Unauthorized" :
        status == 400 ? "400 Bad Request" :
        status == 409 ? "409 Conflict" : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(r);
    return ESP_FAIL;
}

static esp_err_t auth_guard(httpd_req_t *req)
{
    if (!authenticate_request(req))
        return send_error(req, 401, "invalid_token", "Token invalido o ausente.");
    return ESP_OK;
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    cJSON *json = receiver_info_get_json();
    char *str = cJSON_PrintUnformatted(json);
    httpd_resp_sendstr(req, str);
    free(str);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t firmware_get_handler(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "device_id", CONFIG_DEVICE_ID_STR);
    cJSON_AddStringToObject(r, "current_version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(r, "build_date", BUILD_DATE);
    cJSON_AddBoolToObject(r, "ota_supported", OTA_SUPPORTED);
    char *str = cJSON_PrintUnformatted(r);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    free(str);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t pair_start_handler(httpd_req_t *req)
{
    char buf[512] = {0};
    httpd_req_recv(req, buf, sizeof(buf) - 1);
    cJSON *body = cJSON_Parse(buf);
    if (!body || !cJSON_GetObjectItem(body, "initiator_id")) {
        if (body) cJSON_Delete(body);
        return send_error(req, 400, "bad_request", "Falta initiator_id.");
    }
    const char *id = cJSON_GetObjectItem(body, "initiator_id")->valuestring;
    char nonce[32] = {0};
    if (!pairing_start(id, nonce, sizeof(nonce))) {
        cJSON_Delete(body);
        return send_error(req, 409, "pairing_window_open", "Ventana de pairing ya activa.");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "pairing_window_open");
    cJSON_AddStringToObject(r, "device_id", CONFIG_DEVICE_ID_STR);
    cJSON_AddNumberToObject(r, "pairing_window_seconds", 120);
    cJSON_AddStringToObject(r, "nonce", nonce);
    char *str = cJSON_PrintUnformatted(r);
    httpd_resp_sendstr(req, str);
    free(str);
    cJSON_Delete(r);
    cJSON_Delete(body);
    return ESP_OK;
}

static esp_err_t pair_confirm_handler(httpd_req_t *req)
{
    char buf[512] = {0};
    httpd_req_recv(req, buf, sizeof(buf) - 1);
    cJSON *body = cJSON_Parse(buf);
    if (!body || !cJSON_GetObjectItem(body, "nonce") ||
        !cJSON_GetObjectItem(body, "initiator_id") ||
        !cJSON_GetObjectItem(body, "token")) {
        if (body) cJSON_Delete(body);
        return send_error(req, 400, "bad_request", "Faltan nonce, initiator_id, token.");
    }
    bool ok = pairing_confirm(
        cJSON_GetObjectItem(body, "nonce")->valuestring,
        cJSON_GetObjectItem(body, "initiator_id")->valuestring,
        cJSON_GetObjectItem(body, "token")->valuestring);
    cJSON_Delete(body);
    if (!ok) return send_error(req, 409, "pairing_window_closed", "Ventana cerrada o nonce invalido.");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "paired");
    cJSON_AddStringToObject(r, "device_id", CONFIG_DEVICE_ID_STR);
    cJSON_AddStringToObject(r, "controller_id", cJSON_GetObjectItem(body, "initiator_id")->valuestring);
    cJSON_AddStringToObject(r, "token", cJSON_GetObjectItem(body, "token")->valuestring);
    char *str = cJSON_PrintUnformatted(r);
    httpd_resp_sendstr(req, str);
    free(str);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t heartbeat_post_handler(httpd_req_t *req)
{
    if (auth_guard(req) != ESP_OK) return ESP_FAIL;
    heartbeat_reset();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "alive");
    cJSON_AddNumberToObject(r, "uptime_seconds", (int)(heartbeat_get_uptime_ms() / 1000));
    char *str = cJSON_PrintUnformatted(r);
    httpd_resp_sendstr(req, str);
    free(str);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t session_start_post_handler(httpd_req_t *req)
{
    if (auth_guard(req) != ESP_OK) return ESP_FAIL;
    char buf[1024] = {0};
    httpd_req_recv(req, buf, sizeof(buf) - 1);
    cJSON *body = cJSON_Parse(buf);
    if (!body) return send_error(req, 400, "bad_request", "JSON invalido.");
    cJSON *i;
    #define get_str(f) (i = cJSON_GetObjectItem(body, f)) ? i->valuestring : ""
    #define get_int(f) (i = cJSON_GetObjectItem(body, f)) ? i->valueint : 0
    const char *sid = get_str("session_id");
    const char *codec = get_str("codec");
    int sr = get_int("sample_rate"), bd = get_int("bit_depth");
    int ch = get_int("channels"), sp = get_int("stream_port");
    int bm = get_int("buffer_ms"), vol = get_int("volume");
    #undef get_str
    #undef get_int
    cJSON_Delete(body);
    if (!*sid || !*codec || sr == 0 || bd == 0 || ch == 0 || sp == 0 || bm == 0)
        return send_error(req, 400, "bad_request", "Faltan campos requeridos.");
    if (sp < 1024 || sp > 65535)
        return send_error(req, 400, "bad_request", "stream_port debe estar entre 1024 y 65535.");
    if (vol < 0 || vol > 100)
        return send_error(req, 400, "bad_request", "volume debe estar entre 0 y 100.");
    if (session_is_active())
        return send_error(req, 409, "session_active", "Ya hay sesion activa.");
    if (!session_start(sid, codec, sr, bd, ch, sp, bm, vol))
        return send_error(req, 400, "unsupported_codec", "Codec o parametros no soportados.");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "session_started");
    cJSON_AddStringToObject(r, "session_id", sid);
    cJSON_AddStringToObject(r, "device_id", CONFIG_DEVICE_ID_STR);
    cJSON_AddNumberToObject(r, "stream_port", sp);
    cJSON_AddNumberToObject(r, "buffer_ms", bm);
    char *str = cJSON_PrintUnformatted(r);
    httpd_resp_sendstr(req, str);
    free(str);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t session_stop_post_handler(httpd_req_t *req)
{
    if (auth_guard(req) != ESP_OK) return ESP_FAIL;
    if (!session_stop()) return send_error(req, 409, "no_active_session", "No hay sesion activa.");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "session_stopped");
    char *str = cJSON_PrintUnformatted(r);
    httpd_resp_sendstr(req, str);
    free(str);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t volume_post_handler(httpd_req_t *req)
{
    if (auth_guard(req) != ESP_OK) return ESP_FAIL;
    char buf[256] = {0};
    httpd_req_recv(req, buf, sizeof(buf) - 1);
    cJSON *body = cJSON_Parse(buf);
    if (!body || !cJSON_GetObjectItem(body, "volume")) {
        if (body) cJSON_Delete(body);
        return send_error(req, 400, "bad_request", "Falta volume.");
    }
    int vol = cJSON_GetObjectItem(body, "volume")->valueint;
    volume_set(vol);
    cJSON_Delete(body);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "volume_set");
    cJSON_AddNumberToObject(r, "volume", vol);
    char *str = cJSON_PrintUnformatted(r);
    httpd_resp_sendstr(req, str);
    free(str);
    cJSON_Delete(r);
    return ESP_OK;
}

#if OTA_SUPPORTED
static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (auth_guard(req) != ESP_OK) return ESP_FAIL;
    char buf[512] = {0};
    httpd_req_recv(req, buf, sizeof(buf) - 1);
    cJSON *body = cJSON_Parse(buf);
    if (!body || !cJSON_GetObjectItem(body, "url")) {
        if (body) cJSON_Delete(body);
        return send_error(req, 400, "bad_request", "Falta campo 'url'.");
    }
    const char *url = cJSON_GetObjectItem(body, "url")->valuestring;
    bool ok = ota_start(url);
    cJSON_Delete(body);
    if (!ok) return send_error(req, 500, "internal_error", "OTA start failed.");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ota_started");
    char *str = cJSON_PrintUnformatted(r);
    httpd_resp_sendstr(req, str);
    free(str);
    cJSON_Delete(r);
    return ESP_OK;
}
#endif

static const httpd_uri_t endpoints[] = {
    {.uri = "/api/v1/receiver/info",        .method = HTTP_GET,  .handler = info_get_handler},
    {.uri = "/api/v1/receiver/firmware",    .method = HTTP_GET,  .handler = firmware_get_handler},
    {.uri = "/api/v1/receiver/pair/start",  .method = HTTP_POST, .handler = pair_start_handler},
    {.uri = "/api/v1/receiver/pair/confirm",.method = HTTP_POST, .handler = pair_confirm_handler},
    {.uri = "/api/v1/receiver/heartbeat",   .method = HTTP_POST, .handler = heartbeat_post_handler},
    {.uri = "/api/v1/receiver/session/start",.method = HTTP_POST,.handler = session_start_post_handler},
    {.uri = "/api/v1/receiver/session/stop", .method = HTTP_POST,.handler = session_stop_post_handler},
    {.uri = "/api/v1/receiver/volume",      .method = HTTP_POST, .handler = volume_post_handler},
#if OTA_SUPPORTED
    {.uri = "/api/v1/receiver/ota",         .method = HTTP_POST, .handler = ota_post_handler},
#endif
};

void michi_link_lite_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = MICHI_REST_PORT;
    cfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));
}

void michi_link_lite_register_endpoints(httpd_handle_t server)
{
    for (int i = 0; i < sizeof(endpoints)/sizeof(endpoints[0]); i++)
        httpd_register_uri_handler(server, &endpoints[i]);
}

bool michi_link_lite_validate_token(const char *token) { return pairing_validate_token(token); }
httpd_handle_t michi_link_lite_get_server(void) { return s_server; }

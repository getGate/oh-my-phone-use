#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "hid_device.h"
#include "input_http.h"
#include "input_player.h"
#include "input_records.h"
#include "input_secret.h"
#include "wifi_link.h"

static const char *TAG = "http";

#define DEVICE_NAME      "PhoneUse ESP32"
#define TRANSPORT_NAME   "Wi-Fi + BLE"
#define SECRET_HEADER    "X-Input-Device-Secret"
#define STATUS_202       "202 Accepted"
#define STATUS_403       "403 Forbidden"
#define STATUS_405       "405 Method Not Allowed"
#define STATUS_409       "409 Conflict"
#define STATUS_413       "413 Payload Too Large"

static httpd_handle_t s_server;
/* The server task handles one request at a time, so these buffers are not shared. */
static char s_body[INPUT_HEX_MAX + 256];
static input_record_t s_records[INPUT_RECORDS_MAX];

/* ---- response helpers ----------------------------------------------------------------- */

static void common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    common_headers(req);
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_empty(httpd_req_t *req, const char *status)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    common_headers(req);
    return httpd_resp_send(req, NULL, 0);
}

/* Copy src into dst as a JSON string body (no surrounding quotes), escaping as needed. */
static void json_escape(char *dst, size_t size, const char *src)
{
    size_t w = 0;
    for (; src && *src && w + 7 < size; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[w++] = '\\';
            dst[w++] = (char)c;
        } else if (c < 0x20) {
            w += (size_t)snprintf(dst + w, size - w, "\\u%04x", c);
        } else {
            dst[w++] = (char)c;
        }
    }
    dst[w] = '\0';
}

static bool authorized(httpd_req_t *req)
{
    char header[INPUT_SECRET_MAX_LEN + 1] = { 0 };
    esp_err_t err = httpd_req_get_hdr_value_str(req, SECRET_HEADER, header, sizeof(header));
    if (err == ESP_OK && input_secret_matches(header)) {
        return true;
    }
    send_json(req, STATUS_403, "{\"error\":\"Input device secret required.\"}");
    return false;
}

/* ---- handlers ------------------------------------------------------------------------- */

static esp_err_t root_get(httpd_req_t *req)
{
    return send_json(req, HTTPD_200,
                     "{\"device\":\"" DEVICE_NAME " Input Tool\",\"board\":\"ESP32-PICO-KIT\","
                     "\"transport\":\"" TRANSPORT_NAME "\",\"status\":\"/status\",\"input\":\"/input\",\"stop\":\"/stop\"}");
}

static esp_err_t status_get(httpd_req_t *req)
{
    hid_status_t hid;
    input_player_status_t player;
    wifi_link_status_t wifi;
    hid_get_status(&hid);
    input_player_get_status(&player);
    wifi_link_get_status(&wifi);

    bool hid_ready = hid.connected;
    bool secret = input_secret_configured();
    const char *hint = NULL;
    if (!hid_ready) {
        hint = hid.bonded_peers > 0
               ? "Waiting for the paired phone to reconnect over Bluetooth (Settings > Bluetooth > " CONFIG_PHONEUSE_HID_DEVICE_NAME ")"
               : "Pair \"" CONFIG_PHONEUSE_HID_DEVICE_NAME "\" in Settings > Bluetooth (on an iPhone, also enable AssistiveTouch)";
    } else if (!secret) {
        hint = "Input-device secret not set on the ESP32: run tools/provision.sh (serial command SECRET)";
    }

    char hint_json[320] = "";
    if (hint) {
        char escaped[256];
        json_escape(escaped, sizeof(escaped), hint);
        snprintf(hint_json, sizeof(hint_json), ",\"hint\":\"%s\"", escaped);
    }
    char ssid[80], ip[24];
    json_escape(ssid, sizeof(ssid), wifi.configured ? wifi.ssid : "");
    json_escape(ip, sizeof(ip), wifi.connected ? wifi.ip : "");

    char json[768];
    snprintf(json, sizeof(json),
             "{\"device\":\"" DEVICE_NAME "\",\"transport\":\"" TRANSPORT_NAME "\","
             "\"running\":%s,\"state\":\"%s\",\"waitMs\":%lu,\"hidReady\":%s,\"reports\":%lu,"
             "\"bleConnected\":%s,\"bleAdvertising\":%s,\"bonds\":%d,\"buttons\":%u,"
             "\"secretConfigured\":%s,\"wifiConnected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"hostname\":\"%s.local\",\"batches\":%lu,\"uptime\":%lu%s}",
             player.running ? "true" : "false", input_player_state_name(player.state),
             (unsigned long)player.wait_ms, hid_ready ? "true" : "false", (unsigned long)hid.reports_sent,
             hid.connected ? "true" : "false", hid.advertising ? "true" : "false", hid.bonded_peers,
             (unsigned)hid.mouse_buttons, secret ? "true" : "false", wifi.connected ? "true" : "false",
             ssid, ip, wifi.connected ? wifi.rssi : 0, CONFIG_PHONEUSE_WIFI_HOSTNAME,
             (unsigned long)player.batches, (unsigned long)(esp_timer_get_time() / 1000000LL), hint_json);
    return send_json(req, HTTPD_200, json);
}

static esp_err_t read_body(httpd_req_t *req, size_t *len)
{
    size_t total = req->content_len;
    if (total >= sizeof(s_body)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, s_body + received, total - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)n;
    }
    s_body[received] = '\0';
    *len = received;
    return ESP_OK;
}

static esp_err_t input_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    input_player_status_t player;
    input_player_get_status(&player);
    if (player.running) {
        return send_json(req, STATUS_409, "{\"error\":\"A sequence is already running.\"}");
    }
    if (!hid_is_connected()) {
        return send_json(req, STATUS_409,
                         "{\"error\":\"Bluetooth HID host is not connected. Pair the phone with the input tool first.\"}");
    }

    size_t len = 0;
    esp_err_t err = read_body(req, &len);
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_json(req, STATUS_413, "{\"error\":\"Invalid input records\"}");
    }
    if (err != ESP_OK) {
        return send_json(req, HTTPD_400, "{\"error\":\"Request body could not be read\"}");
    }

    /* Trim surrounding whitespace, like String::trim() in the reference firmware. */
    char *hex = s_body;
    while (len > 0 && isspace((unsigned char)hex[len - 1])) {
        hex[--len] = '\0';
    }
    while (len > 0 && isspace((unsigned char)*hex)) {
        hex++;
        len--;
    }

    size_t count = 0;
    switch (input_records_decode(hex, len, s_records, &count)) {
    case INPUT_HEX_BAD_LENGTH:
        return send_json(req, HTTPD_400, "{\"error\":\"Invalid input records\"}");
    case INPUT_HEX_BAD_DIGIT:
        return send_json(req, HTTPD_400, "{\"error\":\"Invalid input encoding\"}");
    default:
        break;
    }
    if (!input_records_validate(s_records, count)) {
        return send_json(req, HTTPD_400, "{\"error\":\"Invalid input records or unreleased button\"}");
    }
    err = input_player_submit(s_records, count);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json(req, STATUS_409, "{\"error\":\"A sequence is already running.\"}");
    }
    if (err != ESP_OK) {
        return send_json(req, HTTPD_500, "{\"error\":\"Could not queue the batch\"}");
    }
    ESP_LOGI(TAG, "queued %u records", (unsigned)count);
    return send_empty(req, STATUS_202);
}

static esp_err_t stop_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    input_player_stop();
    ESP_LOGI(TAG, "stop requested");
    return send_empty(req, HTTPD_200);
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    return send_json(req, HTTPD_404, "{\"error\":\"Not found\"}");
}

static esp_err_t method_not_allowed(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    return send_json(req, STATUS_405, "{\"error\":\"Method not allowed\"}");
}

/* ---- lifecycle ------------------------------------------------------------------------ */

esp_err_t input_http_start(void)
{
    ESP_RETURN_ON_FALSE(s_server == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_PHONEUSE_INPUT_TOOL_HTTP_PORT;
    config.stack_size = 6144;
    config.max_open_sockets = 4;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd start");

    const httpd_uri_t routes[] = {
        { .uri = "/",       .method = HTTP_GET,  .handler = root_get },
        { .uri = "/status", .method = HTTP_GET,  .handler = status_get },
        { .uri = "/input",  .method = HTTP_POST, .handler = input_post },
        { .uri = "/stop",   .method = HTTP_POST, .handler = stop_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG, "route %s", routes[i].uri);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);
    httpd_register_err_handler(s_server, HTTPD_405_METHOD_NOT_ALLOWED, method_not_allowed);

    ESP_LOGI(TAG, "input-tool HTTP API on port %d", CONFIG_PHONEUSE_INPUT_TOOL_HTTP_PORT);
    return ESP_OK;
}

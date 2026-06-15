/*
 * HTTP REST server for wireless file upload and print control.
 *
 * Endpoints:
 *   GET  /                          → HTML status page (browser friendly)
 *   GET  /api/status                → JSON printer status
 *   GET  /api/files                 → JSON folder list
 *   PUT  /api/files/{folder}/{file} → upload a layer PNG to SD card
 *   POST /api/print/start           → body: {"folder":"name"} start print
 *   POST /api/print/pause           → pause current print
 *   POST /api/print/resume          → resume paused print
 *   POST /api/print/cancel          → cancel print
 *   POST /api/wifi                  → body: {"ssid":"x","password":"y"}
 *   GET  /api/wifi/status           → current WiFi state + IP
 *
 * File upload workflow (e.g. via curl):
 *   curl -X PUT http://lite3dp-g2.local/api/files/MyPrint/lychee0000.png \
 *        --data-binary @layer0.png
 *
 * Then start the print:
 *   curl -X POST http://lite3dp-g2.local/api/print/start \
 *        -d '{"folder":"MyPrint"}'
 */

#include "http_server.h"
#include "shared.h"
#include "wifi_mgr.h"
#include "print_core.h"   // sd_mount, sd_card_present
#include "slicer.h"       // slicer_count_layers
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

static const char *TAG = "http";
static httpd_handle_t s_server = NULL;

// Chunk size for streaming file bodies from HTTP to SD
#define UPLOAD_CHUNK 1024

// ── Helpers ───────────────────────────────────────────────────────────────

static const char *print_state_str(void) {
    switch (g_print_state) {
        case PRINT_STATE_IDLE:     return "idle";
        case PRINT_STATE_HOMING:   return "homing";
        case PRINT_STATE_PRINTING: return "printing";
        case PRINT_STATE_PAUSED:   return "paused";
        case PRINT_STATE_ERROR:    return "error";
        default:                   return "unknown";
    }
}

// Reject paths containing ".." or absolute separators to prevent traversal.
static bool path_safe(const char *s) {
    if (!s || s[0] == '\0') return false;
    if (strstr(s, "..")) return false;
    if (strchr(s, '/'))   return false;   // must be a single component
    return true;
}

static esp_err_t send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, json);
}

// ── GET / ─────────────────────────────────────────────────────────────────

static esp_err_t handle_root(httpd_req_t *req) {
    char ip[16];
    wifi_get_ip(ip, sizeof(ip));
    char html[1024];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><title>Lite3DP Gen-2</title></head><body>"
        "<h2>Lite3DP Gen-2</h2>"
        "<p>State: <b>%s</b></p>"
        "<p>Layer: %d / %d</p>"
        "<p>IP: %s</p>"
        "<hr>"
        "<p>API: <code>GET /api/status</code> &nbsp; "
        "<code>PUT /api/files/{folder}/{file}</code> &nbsp; "
        "<code>POST /api/print/start</code></p>"
        "<p>See README for slicer configuration.</p>"
        "</body></html>",
        print_state_str(),
        (int)g_layer_current, (int)g_layer_total,
        ip[0] ? ip : "n/a");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, html);
}

// ── GET /api/status ───────────────────────────────────────────────────────

static esp_err_t handle_status(httpd_req_t *req) {
    char ip[16]; wifi_get_ip(ip, sizeof(ip));
    char json[192];
    snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"layer\":%d,\"total\":%d,\"ip\":\"%s\"}",
        print_state_str(),
        (int)g_layer_current,
        (int)g_layer_total,
        ip[0] ? ip : "");
    return send_json(req, json);
}

// ── GET /api/files ────────────────────────────────────────────────────────

static esp_err_t handle_list_files(httpd_req_t *req) {
    if (!sd_mount()) {
        httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, "SD not ready");
        return ESP_OK;
    }

    char names[64][25];
    int n = sd_list_folders(names, 64);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr_chunk(req, "{\"folders\":[");
    for (int i = 0; i < n; i++) {
        char item[32];
        snprintf(item, sizeof(item), "%s\"%s\"", i ? "," : "", names[i]);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ── PUT /api/files/{folder}/{filename} ────────────────────────────────────

static esp_err_t handle_upload(httpd_req_t *req) {
    // URI: /api/files/{folder}/{filename}
    const char *uri = req->uri;
    const char *prefix = "/api/files/";
    if (strncmp(uri, prefix, strlen(prefix)) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad URI");
        return ESP_OK;
    }

    // Parse folder and filename from URI
    char folder[25] = "", filename[64] = "";
    const char *rest = uri + strlen(prefix);
    const char *slash = strchr(rest, '/');
    if (!slash) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing filename");
        return ESP_OK;
    }
    size_t flen = (size_t)(slash - rest);
    if (flen >= sizeof(folder)) flen = sizeof(folder) - 1;
    memcpy(folder, rest, flen); folder[flen] = '\0';
    strncpy(filename, slash + 1, sizeof(filename) - 1);

    if (!path_safe(folder) || !path_safe(filename)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsafe path");
        return ESP_OK;
    }

    if (!sd_mount()) {
        httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, "SD not ready");
        return ESP_OK;
    }

    // Create folder if needed
    char dir[64];
    snprintf(dir, sizeof(dir), "/sdcard/%s", folder);
    mkdir(dir, 0755);   // OK if already exists

    // Open output file
    char path[128];
    snprintf(path, sizeof(path), "/sdcard/%s/%s", folder, filename);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "open failed: %s (%d)", path, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create file");
        return ESP_OK;
    }

    // Stream body to file
    char buf[UPLOAD_CHUNK];
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int to_read = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
        int got = httpd_req_recv(req, buf, to_read);
        if (got <= 0) { ok = false; break; }
        if ((int)fwrite(buf, 1, got, fp) != got) { ok = false; break; }
        remaining -= got;
    }
    fclose(fp);

    if (!ok) {
        remove(path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "uploaded: %s", path);
    return send_json(req, "{\"ok\":true}");
}

// ── POST /api/print/start ─────────────────────────────────────────────────

static esp_err_t handle_print_start(httpd_req_t *req) {
    if (g_print_state != PRINT_STATE_IDLE) {
        httpd_resp_send_err(req, HTTPD_409_CONFLICT, "printer busy");
        return ESP_OK;
    }

    // Read body: {"folder":"name"}
    char body[64] = "";
    int len = (req->content_len < (int)sizeof(body) - 1)
                ? req->content_len : (int)sizeof(body) - 1;
    if (len > 0) httpd_req_recv(req, body, len);

    // Minimal JSON parse: find "folder" value
    char *fp = strstr(body, "\"folder\"");
    if (!fp) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing folder");
        return ESP_OK;
    }
    fp = strchr(fp, ':'); if (!fp) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "parse error"); return ESP_OK; }
    fp = strchr(fp, '"'); if (!fp) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "parse error"); return ESP_OK; }
    fp++;
    char *ep = strchr(fp, '"'); if (!ep) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "parse error"); return ESP_OK; }
    *ep = '\0';

    if (!path_safe(fp)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsafe folder name");
        return ESP_OK;
    }

    if (!sd_mount()) {
        httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, "SD not ready");
        return ESP_OK;
    }

    int layer_count = slicer_count_layers(fp);
    if (layer_count == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no layers found");
        return ESP_OK;
    }

    print_cmd_t cmd = { .type = CMD_PRINT_START };
    strncpy(cmd.start.folder, fp, sizeof(cmd.start.folder) - 1);
    cmd.start.layer_count = layer_count;
    cmd.start.print_mode  = 0;   // SD mode

    if (xQueueSend(g_print_cmd_queue, &cmd, 0) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_409_CONFLICT, "queue full");
        return ESP_OK;
    }

    return send_json(req, "{\"ok\":true}");
}

// ── POST /api/print/{pause,resume,cancel} ────────────────────────────────

static esp_err_t handle_print_ctrl(httpd_req_t *req) {
    const char *uri = req->uri;
    print_cmd_t cmd = {};
    if      (strstr(uri, "pause"))  cmd.type = CMD_PRINT_PAUSE;
    else if (strstr(uri, "resume")) cmd.type = CMD_PRINT_RESUME;
    else if (strstr(uri, "cancel")) cmd.type = CMD_PRINT_CANCEL;
    else { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown command"); return ESP_OK; }

    xQueueSend(g_print_cmd_queue, &cmd, 0);
    return send_json(req, "{\"ok\":true}");
}

// ── POST /api/wifi ────────────────────────────────────────────────────────

static esp_err_t handle_wifi_set(httpd_req_t *req) {
    char body[128] = "";
    int len = (req->content_len < (int)sizeof(body) - 1)
                ? req->content_len : (int)sizeof(body) - 1;
    if (len > 0) httpd_req_recv(req, body, len);

    // Minimal JSON parse: {"ssid":"x","password":"y"}
    char ssid[33] = "", pass[65] = "";

    char *sp = strstr(body, "\"ssid\"");
    if (sp) { sp = strchr(sp, ':'); sp = strchr(sp, '"'); sp++; char *e = strchr(sp, '"'); if (e) { *e='\0'; strncpy(ssid, sp, 32); *e='"'; } }

    char *pp = strstr(body, "\"password\"");
    if (pp) { pp = strchr(pp, ':'); pp = strchr(pp, '"'); pp++; char *e = strchr(pp, '"'); if (e) { *e='\0'; strncpy(pass, pp, 64); *e='"'; } }

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_OK;
    }

    wifi_set_credentials(ssid, pass);
    return send_json(req, "{\"ok\":true,\"msg\":\"connecting\"}");
}

// ── GET /api/wifi/status ──────────────────────────────────────────────────

static esp_err_t handle_wifi_status(httpd_req_t *req) {
    char ip[16]; wifi_get_ip(ip, sizeof(ip));
    char ssid[33]; wifi_get_ssid(ssid, sizeof(ssid));
    const char *state;
    switch (wifi_get_state()) {
        case WIFI_STATE_CONNECTED:    state = "connected";    break;
        case WIFI_STATE_CONNECTING:   state = "connecting";   break;
        case WIFI_STATE_AP_MODE:      state = "ap_mode";      break;
        default:                      state = "disconnected";  break;
    }
    char json[128];
    snprintf(json, sizeof(json),
        "{\"wifi_state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\"}",
        state, ssid, ip[0] ? ip : "");
    return send_json(req, json);
}

// ── Server lifecycle ──────────────────────────────────────────────────────

void http_server_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn    = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 12;
    cfg.stack_size       = 8192;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server");
        return;
    }

    static const httpd_uri_t routes[] = {
        { "/",                    HTTP_GET,  handle_root,        NULL },
        { "/api/status",          HTTP_GET,  handle_status,      NULL },
        { "/api/files",           HTTP_GET,  handle_list_files,  NULL },
        { "/api/files/*",         HTTP_PUT,  handle_upload,      NULL },
        { "/api/print/start",     HTTP_POST, handle_print_start, NULL },
        { "/api/print/pause",     HTTP_POST, handle_print_ctrl,  NULL },
        { "/api/print/resume",    HTTP_POST, handle_print_ctrl,  NULL },
        { "/api/print/cancel",    HTTP_POST, handle_print_ctrl,  NULL },
        { "/api/wifi",            HTTP_POST, handle_wifi_set,    NULL },
        { "/api/wifi/status",     HTTP_GET,  handle_wifi_status, NULL },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    ESP_LOGI(TAG, "HTTP server started on port 80");
}

void http_server_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

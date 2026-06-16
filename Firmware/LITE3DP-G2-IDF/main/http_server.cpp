/*
 * HTTP REST server for wireless file upload and print control.
 *
 * Endpoints:
 *   GET  /                          → HTML status/setup page
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
 *   curl -X POST http://lite3dp-g2.local/api/print/start \
 *        -d '{"folder":"MyPrint"}'
 */

#include "http_server.h"
#include "shared.h"
#include "wifi_mgr.h"
#include "print_core.h"
#include "slicer.h"
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

#define UPLOAD_CHUNK 1024

// ── Helpers ───────────────────────────────────────────────────────────────

// esp_http_server only defines a small set of httpd_err_code_t values.
// For other status codes we set the status line manually.
static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

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

static bool path_safe(const char *s) {
    if (!s || s[0] == '\0') return false;
    if (strstr(s, "..")) return false;
    if (strchr(s, '/'))   return false;
    return true;
}

static esp_err_t send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, json);
}

// ── GET / ─────────────────────────────────────────────────────────────────
// In AP mode: shows a credential entry form.
// In STA mode: shows printer status.

static esp_err_t handle_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");

    if (wifi_get_state() == WIFI_STATE_AP_MODE) {
        // Provisioning page — served at 192.168.4.1 when in AP mode
        const char *html =
            "<!DOCTYPE html><html><head><title>Lite3DP WiFi Setup</title>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>body{font-family:sans-serif;max-width:420px;margin:40px auto;padding:0 16px}"
            "h2{color:#333}input{width:100%;padding:8px;margin:6px 0;box-sizing:border-box}"
            "button{width:100%;padding:10px;background:#2196F3;color:#fff;border:none;"
            "border-radius:4px;font-size:16px;cursor:pointer}"
            "button:hover{background:#1976D2}.ok{color:green}.err{color:red}</style></head>"
            "<body><h2>Lite3DP Gen-2 WiFi Setup</h2>"
            "<p>Enter your home WiFi credentials. The printer will reboot and connect.</p>"
            "<form id='f'>"
            "<label>SSID<input id='s' name='ssid' placeholder='Network name' required></label>"
            "<label>Password<input id='p' name='password' type='password' placeholder='Password'></label>"
            "<br><br><button type='submit'>Save &amp; Connect</button>"
            "</form><p id='msg'></p>"
            "<script>"
            "document.getElementById('f').onsubmit=function(e){"
            "e.preventDefault();"
            "var b=JSON.stringify({ssid:document.getElementById('s').value,"
            "password:document.getElementById('p').value});"
            "fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:b})"
            ".then(r=>r.json()).then(d=>{"
            "document.getElementById('msg').className='ok';"
            "document.getElementById('msg').textContent='Saved! Printer is connecting...';"
            "}).catch(()=>{"
            "document.getElementById('msg').className='err';"
            "document.getElementById('msg').textContent='Error — try again.';"
            "});}"
            "</script></body></html>";
        return httpd_resp_sendstr(req, html);
    }

    char ip[16];  wifi_get_ip(ip, sizeof(ip));
    char html[1024];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><title>Lite3DP Gen-2</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px}"
        "h2{color:#333}.state{font-size:1.4em;font-weight:bold}"
        "code{background:#eee;padding:2px 6px;border-radius:3px}</style></head>"
        "<body><h2>Lite3DP Gen-2</h2>"
        "<p>State: <span class='state'>%s</span></p>"
        "<p>Layer: <b>%d / %d</b></p>"
        "<p>IP: <code>%s</code> &nbsp; "
        "<a href='http://lite3dp-g2.local'>lite3dp-g2.local</a></p>"
        "<hr><h3>Quick API</h3>"
        "<pre>GET  /api/status\n"
        "GET  /api/files\n"
        "PUT  /api/files/{folder}/{file}\n"
        "POST /api/print/start  {\"folder\":\"name\"}\n"
        "POST /api/print/pause|resume|cancel</pre>"
        "</body></html>",
        print_state_str(),
        (int)g_layer_current, (int)g_layer_total,
        ip[0] ? ip : "n/a");
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
        return send_error(req, "503 Service Unavailable", "SD not ready");
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
    const char *uri    = req->uri;
    const char *prefix = "/api/files/";
    if (strncmp(uri, prefix, strlen(prefix)) != 0) {
        return send_error(req, "400 Bad Request", "bad URI");
    }

    char folder[25] = "", filename[64] = "";
    const char *rest  = uri + strlen(prefix);
    const char *slash = strchr(rest, '/');
    if (!slash) {
        return send_error(req, "400 Bad Request", "missing filename");
    }
    size_t flen = (size_t)(slash - rest);
    if (flen >= sizeof(folder)) flen = sizeof(folder) - 1;
    memcpy(folder, rest, flen); folder[flen] = '\0';
    strncpy(filename, slash + 1, sizeof(filename) - 1);

    if (!path_safe(folder) || !path_safe(filename)) {
        return send_error(req, "400 Bad Request", "unsafe path");
    }
    if (!sd_mount()) {
        return send_error(req, "503 Service Unavailable", "SD not ready");
    }

    char dir[64];
    snprintf(dir, sizeof(dir), "/sdcard/%s", folder);
    mkdir(dir, 0755);

    char path[128];
    snprintf(path, sizeof(path), "/sdcard/%s/%s", folder, filename);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "open failed: %s (%d)", path, errno);
        return send_error(req, "500 Internal Server Error", "cannot create file");
    }

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
        return send_error(req, "500 Internal Server Error", "upload incomplete");
    }
    ESP_LOGI(TAG, "uploaded: %s", path);
    return send_json(req, "{\"ok\":true}");
}

// ── POST /api/print/start ─────────────────────────────────────────────────

static esp_err_t handle_print_start(httpd_req_t *req) {
    if (g_print_state != PRINT_STATE_IDLE) {
        return send_error(req, "409 Conflict", "printer busy");
    }

    char body[64] = "";
    int len = (req->content_len < (int)sizeof(body) - 1)
                ? req->content_len : (int)sizeof(body) - 1;
    if (len > 0) httpd_req_recv(req, body, len);

    char *fp = strstr(body, "\"folder\"");
    if (!fp) return send_error(req, "400 Bad Request", "missing folder");
    fp = strchr(fp, ':'); if (!fp) return send_error(req, "400 Bad Request", "parse error");
    fp = strchr(fp, '"'); if (!fp) return send_error(req, "400 Bad Request", "parse error");
    fp++;
    char *ep = strchr(fp, '"'); if (!ep) return send_error(req, "400 Bad Request", "parse error");
    *ep = '\0';

    if (!path_safe(fp)) return send_error(req, "400 Bad Request", "unsafe folder");
    if (!sd_mount())    return send_error(req, "503 Service Unavailable", "SD not ready");

    int layer_count = slicer_count_layers(fp);
    if (layer_count == 0) return send_error(req, "404 Not Found", "no layers found");

    print_cmd_t cmd = { .type = CMD_PRINT_START };
    strncpy(cmd.start.folder, fp, sizeof(cmd.start.folder) - 1);
    cmd.start.layer_count = layer_count;
    cmd.start.print_mode  = 0;

    if (xQueueSend(g_print_cmd_queue, &cmd, 0) != pdTRUE) {
        return send_error(req, "409 Conflict", "queue full");
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
    else return send_error(req, "404 Not Found", "unknown command");

    xQueueSend(g_print_cmd_queue, &cmd, 0);
    return send_json(req, "{\"ok\":true}");
}

// ── POST /api/wifi ────────────────────────────────────────────────────────

static esp_err_t handle_wifi_set(httpd_req_t *req) {
    char body[128] = "";
    int len = (req->content_len < (int)sizeof(body) - 1)
                ? req->content_len : (int)sizeof(body) - 1;
    if (len > 0) httpd_req_recv(req, body, len);

    char ssid[33] = "", pass[65] = "";

    char *sp = strstr(body, "\"ssid\"");
    if (sp) {
        sp = strchr(sp, ':'); if (sp) sp = strchr(sp, '"');
        if (sp) { sp++; char *e = strchr(sp, '"'); if (e) { *e='\0'; strncpy(ssid, sp, 32); *e='"'; } }
    }
    char *pp = strstr(body, "\"password\"");
    if (pp) {
        pp = strchr(pp, ':'); if (pp) pp = strchr(pp, '"');
        if (pp) { pp++; char *e = strchr(pp, '"'); if (e) { *e='\0'; strncpy(pass, pp, 64); *e='"'; } }
    }

    if (ssid[0] == '\0') return send_error(req, "400 Bad Request", "missing ssid");

    wifi_set_credentials(ssid, pass);
    return send_json(req, "{\"ok\":true,\"msg\":\"connecting\"}");
}

// ── GET /api/wifi/status ──────────────────────────────────────────────────

static esp_err_t handle_wifi_status(httpd_req_t *req) {
    char ip[16];   wifi_get_ip(ip,     sizeof(ip));
    char ssid[33]; wifi_get_ssid(ssid, sizeof(ssid));
    const char *state;
    switch (wifi_get_state()) {
        case WIFI_STATE_CONNECTED:  state = "connected";   break;
        case WIFI_STATE_CONNECTING: state = "connecting";  break;
        case WIFI_STATE_AP_MODE:    state = "ap_mode";     break;
        default:                    state = "disconnected"; break;
    }
    char json[128];
    snprintf(json, sizeof(json),
        "{\"wifi_state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\"}",
        state, ssid, ip[0] ? ip : "");
    return send_json(req, json);
}

// ── Server lifecycle ──────────────────────────────────────────────────────

void http_server_start(void) {
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 12;
    cfg.stack_size       = 8192;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server");
        return;
    }

    static const httpd_uri_t routes[] = {
        { "/",                 HTTP_GET,  handle_root,        NULL },
        { "/api/status",       HTTP_GET,  handle_status,      NULL },
        { "/api/files",        HTTP_GET,  handle_list_files,  NULL },
        { "/api/files/*",      HTTP_PUT,  handle_upload,      NULL },
        { "/api/print/start",  HTTP_POST, handle_print_start, NULL },
        { "/api/print/pause",  HTTP_POST, handle_print_ctrl,  NULL },
        { "/api/print/resume", HTTP_POST, handle_print_ctrl,  NULL },
        { "/api/print/cancel", HTTP_POST, handle_print_ctrl,  NULL },
        { "/api/wifi",         HTTP_POST, handle_wifi_set,    NULL },
        { "/api/wifi/status",  HTTP_GET,  handle_wifi_status, NULL },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    ESP_LOGI(TAG, "HTTP server started on port 80");
}

void http_server_stop(void) {
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
}

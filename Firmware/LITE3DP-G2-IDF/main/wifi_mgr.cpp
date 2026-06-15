#include "wifi_mgr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mdns.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG     = "wifi";
static const char *NVS_NS  = "wifi_creds";
static const char *AP_SSID = "Lite3DP-G2";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           5

static volatile wifi_state_t s_state = WIFI_STATE_DISCONNECTED;
static char s_ip[16]   = "";
static char s_ssid[33] = "";
static int  s_retry    = 0;

static EventGroupHandle_t s_wifi_eg;
static esp_netif_t       *s_sta_netif = NULL;
static esp_netif_t       *s_ap_netif  = NULL;

// ── NVS credential helpers ────────────────────────────────────────────────

static bool load_creds(char *ssid, char *pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = 33;
    bool ok = (nvs_get_str(h, "ssid", ssid, &sz) == ESP_OK);
    sz = 65;
    ok &= (nvs_get_str(h, "pass", pass, &sz) == ESP_OK);
    nvs_close(h);
    return ok && ssid[0] != '\0';
}

static void save_creds(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

static void erase_creds(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
}

// ── Event handler ─────────────────────────────────────────────────────────

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            s_state = WIFI_STATE_CONNECTING;
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_retry < MAX_RETRY) {
                s_retry++;
                s_state = WIFI_STATE_CONNECTING;
                esp_wifi_connect();
                ESP_LOGW(TAG, "retry %d/%d", s_retry, MAX_RETRY);
            } else {
                s_state = WIFI_STATE_DISCONNECTED;
                xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
                ESP_LOGW(TAG, "connection failed after %d attempts", MAX_RETRY);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        s_state = WIFI_STATE_CONNECTED;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "connected, IP: %s", s_ip);

        // Advertise via mDNS
        mdns_init();
        mdns_hostname_set("lite3dp-g2");
        mdns_instance_name_set("Lite3DP Gen-2 Printer");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS: lite3dp-g2.local");
    }
}

// ── STA mode ──────────────────────────────────────────────────────────────

static void start_sta(const char *ssid, const char *pass) {
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_retry = 0;
    s_state = WIFI_STATE_CONNECTING;
}

// ── AP mode (captive provisioning) ────────────────────────────────────────

static void start_ap(void) {
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t cfg = {};
    strncpy((char *)cfg.ap.ssid, AP_SSID, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len    = (uint8_t)strlen(AP_SSID);
    cfg.ap.channel     = 1;
    cfg.ap.authmode    = WIFI_AUTH_OPEN;
    cfg.ap.max_connection = 4;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    esp_wifi_start();

    s_state = WIFI_STATE_AP_MODE;
    strncpy(s_ssid, AP_SSID, sizeof(s_ssid) - 1);
    ESP_LOGI(TAG, "AP mode: SSID=%s  192.168.4.1", AP_SSID);
}

// ── Public API ────────────────────────────────────────────────────────────

void wifi_init(void) {
    esp_netif_init();
    esp_event_loop_create_default();

    s_wifi_eg = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,  wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    char ssid[33] = "", pass[65] = "";
    if (load_creds(ssid, pass)) {
        ESP_LOGI(TAG, "found creds for '%s', connecting...", ssid);
        start_sta(ssid, pass);
    } else {
        ESP_LOGI(TAG, "no credentials, starting AP");
        start_ap();
    }
}

wifi_state_t wifi_get_state(void) { return s_state; }

void wifi_get_ip(char *buf, size_t len) {
    strncpy(buf, s_ip, len - 1);
    buf[len - 1] = '\0';
}

void wifi_get_ssid(char *buf, size_t len) {
    strncpy(buf, s_ssid, len - 1);
    buf[len - 1] = '\0';
}

void wifi_set_credentials(const char *ssid, const char *password) {
    save_creds(ssid, password);
    // Reconnect: stop current mode then restart as STA
    esp_wifi_stop();
    start_sta(ssid, password);
}

void wifi_clear_credentials(void) {
    erase_creds();
    esp_wifi_stop();
    start_ap();
}

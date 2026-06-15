#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_AP_MODE,      // SoftAP provisioning mode
} wifi_state_t;

// Initialise WiFi: STA mode if credentials in NVS, AP mode otherwise.
// Must be called after nvs_flash_init().
void wifi_init(void);

wifi_state_t wifi_get_state(void);

// Filled when WIFI_STATE_CONNECTED; empty string otherwise.
void wifi_get_ip(char *buf, size_t len);

// SSID of the station we are (or were last) trying to join.
void wifi_get_ssid(char *buf, size_t len);

// Persist new credentials and reconnect (or switch from AP→STA).
void wifi_set_credentials(const char *ssid, const char *password);

// Erase stored credentials and restart in AP mode.
void wifi_clear_credentials(void);

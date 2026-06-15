#pragma once

// Start the HTTP REST server on port 80.
// Must be called after wifi_init() and SD card is accessible.
void http_server_start(void);

// Stop the server (call before deep-sleep or reboot if needed).
void http_server_stop(void);

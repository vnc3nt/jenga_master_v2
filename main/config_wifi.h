#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Versucht, sich mit dem gespeicherten WiFi zu verbinden.
// Gibt true zurück, wenn erfolgreich, sonst false.
bool connect_saved_wifi(void);

// Startet den Access Point "ESP Configuration WIFI"
void start_config_wifi(void);

// Stoppt WiFi (falls nötig)
void stop_config_wifi(void);

// Exported Handlers for Webserver integration
esp_err_t config_html_handler(httpd_req_t *req);
esp_err_t config_js_handler(httpd_req_t *req);
esp_err_t scan_handler(httpd_req_t *req);
esp_err_t save_handler(httpd_req_t *req);
esp_err_t status_handler(httpd_req_t *req);
esp_err_t mdns_get_handler(httpd_req_t *req);
esp_err_t mdns_set_handler(httpd_req_t *req);
esp_err_t forget_handler(httpd_req_t *req);
esp_err_t restart_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
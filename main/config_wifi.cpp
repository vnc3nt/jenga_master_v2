#include "config_wifi.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/dns.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static const char *TAG = "CONFIG_WIFI";
static httpd_handle_t config_server = NULL;

// --- WiFi connect control ---
static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_event_handler_instance_t s_wifi_instance_any_id = NULL;
static esp_event_handler_instance_t s_ip_instance_got_ip = NULL;
static int s_boot_retry_num = 0;
static bool s_boot_phase = false;
static bool s_reconnect_enabled = false;

static const EventBits_t WIFI_CONNECTED_BIT = BIT0;
static const EventBits_t WIFI_FAIL_BIT = BIT1;

// Forward declarations
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data);

// --- EINGEBETTETE DATEIEN (Deklarationen) ---
extern const unsigned char config_html_start[] asm("_binary_config_html_start");
extern const unsigned char config_html_end[] asm("_binary_config_html_end");
extern const unsigned char config_js_start[] asm("_binary_config_js_start");
extern const unsigned char config_js_end[] asm("_binary_config_js_end");
extern const unsigned char style_css_start[] asm("_binary_style_css_start");
extern const unsigned char style_css_end[] asm("_binary_style_css_end");

static void register_wifi_handlers()
{
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    }

    if (s_wifi_instance_any_id == NULL) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &s_wifi_instance_any_id));
    }
    if (s_ip_instance_got_ip == NULL) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &s_ip_instance_got_ip));
    }
}

// --- EVENT HANDLER ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_reconnect_enabled) {
            return;
        }

        if (s_boot_phase) {
            if (s_boot_retry_num < 3) {
                s_boot_retry_num++;
                ESP_LOGW(TAG, "WiFi disconnected during boot. Retry %d/3", s_boot_retry_num);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "WiFi boot retries exceeded. Giving up.");
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else {
            // Runtime reconnect: keep trying forever
            ESP_LOGI(TAG, "WiFi disconnected at runtime. Reconnecting...");
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_boot_retry_num = 0;
        s_boot_phase = false;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        return;
    }
}

// --- HANDLER ---

// Scan Handler
esp_err_t scan_handler(httpd_req_t *req) {
    wifi_scan_config_t scan_config = {};
    scan_config.ssid = 0;
    scan_config.bssid = 0;
    scan_config.channel = 0;
    scan_config.show_hidden = true;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!ap_list) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(item, "auth", ap_list[i].authmode);
        cJSON_AddItemToArray(root, item);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    free(ap_list);
    return ESP_OK;
}

// Status Handler
esp_err_t status_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    
    // 1. Check Connection
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddBoolToObject(root, "connected", true);
        cJSON_AddStringToObject(root, "connected_ssid", (char *)ap_info.ssid);
        cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
    } else {
        cJSON_AddBoolToObject(root, "connected", false);
    }

    // 2. Check Saved
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        char ssid[33] = {0};
        size_t required_size = sizeof(ssid);
        if (nvs_get_str(my_handle, "wifi_ssid", ssid, &required_size) == ESP_OK) {
            cJSON_AddStringToObject(root, "saved_ssid", ssid);
        }
        nvs_close(my_handle);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static bool is_all_digits(const char *s)
{
    if (s == NULL || *s == '\0') return false;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return false;
        s++;
    }
    return true;
}

// Geräte-ID Handler (GET): { id: number|null }
esp_err_t mdns_get_handler(httpd_req_t *req)
{
    bool has_id = false;
    int32_t id_value = 0;

    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        // Preferred: numeric key
        if (nvs_get_i32(my_handle, "jenga_id", &id_value) == ESP_OK) {
            if (id_value >= 1 && id_value <= 99) {
                has_id = true;
            }
        }

        // Backwards compatibility: legacy string key
        if (!has_id) {
            char legacy[16] = {0};
            size_t len = sizeof(legacy);
            if (nvs_get_str(my_handle, "mdns_id", legacy, &len) == ESP_OK) {
                if (is_all_digits(legacy)) {
                    int v = atoi(legacy);
                    if (v >= 1 && v <= 99) {
                        id_value = v;
                        has_id = true;
                    }
                }
            }
        }

        nvs_close(my_handle);
    }

    cJSON *root = cJSON_CreateObject();
    if (has_id) {
        cJSON_AddNumberToObject(root, "id", id_value);
    } else {
        cJSON_AddNullToObject(root, "id");
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// Geräte-ID Handler (POST): { id: number|null } (null/fehlend => auto)
esp_err_t mdns_set_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Invalid JSON", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");

    bool set_manual = false;
    int32_t manual_value = 0;
    bool valid = false;

    if (id == NULL || cJSON_IsNull(id)) {
        valid = true; // auto
    } else if (cJSON_IsNumber(id)) {
        int v = id->valueint;
        if (v >= 1 && v <= 99) {
            valid = true;
            set_manual = true;
            manual_value = v;
        }
    } else if (cJSON_IsString(id) && id->valuestring != NULL) {
        // Compatibility: allow "auto"/"" or digits
        char tmp[16] = {0};
        strlcpy(tmp, id->valuestring, sizeof(tmp));

        // trim leading spaces
        char *p = tmp;
        while (*p && isspace((unsigned char)*p)) p++;

        for (size_t i = 0; p[i]; i++) {
            p[i] = (char)tolower((unsigned char)p[i]);
        }

        if (*p == '\0' || strcmp(p, "auto") == 0) {
            valid = true;
        } else if (is_all_digits(p)) {
            int v = atoi(p);
            if (v >= 1 && v <= 99) {
                valid = true;
                set_manual = true;
                manual_value = v;
            }
        }
    }

    if (!valid) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "id must be null/empty (auto) or 1..99", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        // Keep storage clean: new numeric key, erase legacy key
        nvs_erase_key(my_handle, "mdns_id");

        if (set_manual) {
            nvs_set_i32(my_handle, "jenga_id", manual_value);
        } else {
            nvs_erase_key(my_handle, "jenga_id");
        }

        nvs_commit(my_handle);
        nvs_close(my_handle);
        httpd_resp_send(req, "OK", 2);
    } else {
        httpd_resp_send_500(req);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// Forget Network Handler
esp_err_t forget_handler(httpd_req_t *req) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_erase_key(my_handle, "wifi_ssid");
        nvs_erase_key(my_handle, "wifi_pass");
        nvs_commit(my_handle);
        nvs_close(my_handle);
        
        // Disconnect immediately
        esp_wifi_disconnect();
        
        httpd_resp_send(req, "OK", 2);
    } else {
        httpd_resp_send_500(req);
    }
    return ESP_OK;
}

// Save Handler
esp_err_t save_handler(httpd_req_t *req) {
    char buf[200];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "password");

    if (ssid && pass) {
        nvs_handle_t my_handle;
        ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &my_handle));
        ESP_ERROR_CHECK(nvs_set_str(my_handle, "wifi_ssid", ssid->valuestring));
        ESP_ERROR_CHECK(nvs_set_str(my_handle, "wifi_pass", pass->valuestring));
        ESP_ERROR_CHECK(nvs_commit(my_handle));
        nvs_close(my_handle);

        // Configure and connect to wifi immediately
        // WICHTIG: Erst trennen, damit der neue Versuch sauber startet
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500)); // Wartezeit erhöht

        wifi_config_t wifi_config = {};
        strncpy((char*)wifi_config.sta.ssid, ssid->valuestring, sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, pass->valuestring, sizeof(wifi_config.sta.password));
        
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

        // In config mode: try connecting, but don't loop forever on wrong password.
        register_wifi_handlers();
        s_boot_retry_num = 0;
        s_boot_phase = true;            // reuse boot-phase retry limit
        s_reconnect_enabled = true;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

        esp_wifi_connect();

        httpd_resp_send(req, "OK", 2);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// Restart Handler
esp_err_t restart_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Restart requested via WebUI");
    httpd_resp_send(req, "OK", 2);
    // Allow time for the response to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// File Handlers
esp_err_t config_html_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)config_html_start, config_html_end - config_html_start);
}
esp_err_t config_js_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/javascript");
    return httpd_resp_send(req, (const char *)config_js_start, config_js_end - config_js_start);
}
static esp_err_t style_css_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, (const char *)style_css_start, style_css_end - style_css_start);
}

// Redirect Handler für Captive Portal Checks
static esp_err_t captive_portal_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// --- DNS SERVER FÜR CAPTIVE PORTAL ---
static TaskHandle_t dns_task_handle = NULL;

static void dns_server_task(void *pvParameters) {
    uint8_t data[512];
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int sock;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(53);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(sock);
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "DNS Server started");

    while (1) {
        int len = recvfrom(sock, data, sizeof(data), 0, (struct sockaddr *)&client_addr, &client_addr_len);
        if (len > 0) {
            // DNS Header manipulieren
            data[2] |= 0x80; // QR Bit
            data[3] &= 0xF0; // RCODE
            data[6] = 0x00; data[7] = 0x01; // Answer Count
            data[8] = 0; data[9] = 0;
            data[10] = 0; data[11] = 0;

            int idx = 12;
            while (data[idx] != 0 && idx < len) {
                idx += data[idx] + 1;
            }
            idx++; 
            idx += 4; 

            // Answer
            data[idx++] = 0xC0; data[idx++] = 0x0C;
            data[idx++] = 0x00; data[idx++] = 0x01;
            data[idx++] = 0x00; data[idx++] = 0x01;
            
            uint32_t ttl = htonl(60);
            memcpy(&data[idx], &ttl, 4);
            idx += 4;
            
            data[idx++] = 0x00; data[idx++] = 0x04;
            
            esp_netif_ip_info_t ip_info;
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            esp_netif_get_ip_info(netif, &ip_info);
            
            memcpy(&data[idx], &ip_info.ip.addr, 4);
            idx += 4;

            sendto(sock, data, idx, 0, (struct sockaddr *)&client_addr, client_addr_len);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    close(sock);
    vTaskDelete(NULL);
}

void start_dns_server() {
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &dns_task_handle);
}

void stop_dns_server() {
    if (dns_task_handle) {
        vTaskDelete(dns_task_handle);
        dns_task_handle = NULL;
    }
}

// Hilfsfunktion zum Registrieren von URIs ohne Warnungen
static void register_uri(httpd_handle_t server, const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *r)) {
    httpd_uri_t u = {}; // Null-Initialisierung
    u.uri = uri;
    u.method = method;
    u.handler = handler;
    u.user_ctx = NULL;
    httpd_register_uri_handler(server, &u);
}

void start_config_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Wir registrieren mehrere URIs (inkl. Captive-Portal Redirects + /mdns)
    config.max_uri_handlers = 20;

    if (httpd_start(&config_server, &config) == ESP_OK) {
        register_uri(config_server, "/", HTTP_GET, config_html_handler);
        register_uri(config_server, "/config", HTTP_GET, config_html_handler);
        register_uri(config_server, "/config.js", HTTP_GET, config_js_handler);
        register_uri(config_server, "/style.css", HTTP_GET, style_css_handler);
        register_uri(config_server, "/scan", HTTP_GET, scan_handler);
        register_uri(config_server, "/save", HTTP_POST, save_handler);
        register_uri(config_server, "/status", HTTP_GET, status_handler);
        register_uri(config_server, "/mdns", HTTP_GET, mdns_get_handler);
        register_uri(config_server, "/mdns", HTTP_POST, mdns_set_handler);
        register_uri(config_server, "/forget", HTTP_POST, forget_handler);
        register_uri(config_server, "/restart", HTTP_POST, restart_handler);

        // Captive Portal Redirects
        register_uri(config_server, "/generate_204", HTTP_GET, captive_portal_handler);
        register_uri(config_server, "/connecttest.txt", HTTP_GET, captive_portal_handler);
        register_uri(config_server, "/hotspot-detect.html", HTTP_GET, captive_portal_handler);
    }
}

void start_config_wifi(void) {
    ESP_LOGI(TAG, "Starting WiFi AP for configuration...");

    // In config mode we don't want background STA connect loops.
    s_reconnect_enabled = false;
    s_boot_phase = false;
    s_boot_retry_num = 0;
    register_wifi_handlers();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Clear STA config so APSTA doesn't try to auto-connect with stale creds
    wifi_config_t sta_config = {};
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    wifi_config_t ap_config = {}; 
    
    strlcpy((char*)ap_config.ap.ssid, "ESP32 Jenga Config", sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen("ESP32 Jenga Config");
    ap_config.ap.password[0] = '\0'; 
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_config_server();
    start_dns_server();
}

void stop_config_wifi(void) {
    stop_dns_server();
    if (config_server) {
        httpd_stop(config_server);
        config_server = NULL;
    }
    esp_wifi_stop();
}

bool connect_saved_wifi(void) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) != ESP_OK) return false;
    
    char ssid[33] = {0};
    char pass[65] = {0};
    size_t s_len = sizeof(ssid);
    size_t p_len = sizeof(pass);
    
    if (nvs_get_str(my_handle, "wifi_ssid", ssid, &s_len) != ESP_OK) {
        nvs_close(my_handle);
        return false;
    }
    nvs_get_str(my_handle, "wifi_pass", pass, &p_len);
    nvs_close(my_handle);

    register_wifi_handlers();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t init_err = esp_wifi_init(&cfg);
    if (init_err != ESP_OK && init_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(init_err);
    }

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); 
    
    wifi_config_t wifi_config = {}; 
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Boot connect: max 3 retries. If it fails -> caller will enter config mode.
    s_boot_retry_num = 0;
    s_boot_phase = true;
    s_reconnect_enabled = true;

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_LOGI(TAG, "Connecting to %s (boot)...", ssid);
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        // Success. From now on: runtime reconnects are allowed.
        s_boot_phase = false;
        s_reconnect_enabled = true;
        return true;
    }

    // Fail or timeout -> stop trying and clean up WiFi before switching to config mode
    ESP_LOGW(TAG, "WiFi connect failed at boot (ssid=%s). Switching to config mode.", ssid);
    s_reconnect_enabled = false;
    s_boot_phase = false;
    esp_wifi_stop();
    esp_wifi_deinit();
    return false;
}
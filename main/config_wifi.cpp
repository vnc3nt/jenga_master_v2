#include "config_wifi.h"
#include "global_vars.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <string.h>

static const char *TAG = "NET";

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!is_master) {
            ESP_LOGW(TAG, "Verbindung verloren. Versuche Reconnect...");
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Verbunden! IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void setup_network() {
    // Sicherstellen, dass alles initialisiert ist
    // esp_netif_init() wird meist schon in main gemacht, aber sicherheitshalber:
    // (Falls in main.cpp schon aufgerufen, ist das hier idempotent oder wir lassen es weg
    // Wir gehen davon aus, dass main.cpp esp_netif_init aufruft)

    if (is_master) {
        // --- MASTER: ACCESS POINT (192.168.4.1) ---
        esp_netif_create_default_wifi_ap();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

        wifi_config_t wifi_config = {};
        strcpy((char*)wifi_config.ap.ssid, "JengaMaster");
        wifi_config.ap.ssid_len = strlen("JengaMaster");
        strcpy((char*)wifi_config.ap.password, ""); // Offenes WLAN
        wifi_config.ap.max_connection = 10;
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        
        ESP_LOGI(TAG, "MASTER Modus: AP 'JengaMaster' gestartet. IP: 192.168.4.1");
    } else {
        // --- CLIENT: STATION ---
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

        wifi_config_t wifi_config = {};
        strcpy((char*)wifi_config.sta.ssid, "JengaMaster");
        strcpy((char*)wifi_config.sta.password, "");
        
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        
        ESP_LOGI(TAG, "CLIENT Modus: Verbinde zu 'JengaMaster'...");
    }
}

// Dummy Funktionen für Kompatibilität mit alten Headers, falls nötig
void start_config_wifi() {}
void stop_config_wifi() {}
bool connect_saved_wifi() { return true; }
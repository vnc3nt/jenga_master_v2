#include "webserver.h"
#include "config_wifi.h"
#include "global_vars.h"
#include "udp_sync.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_http_client.h" 
#include "cJSON.h"
#include <esp_netif.h>
#include <esp_event.h>
#include "esp_mac.h"

static const char *TAG = "MAIN";

uint32_t my_client_id = 0;

SemaphoreHandle_t game_mutex = NULL;
std::vector<Robot> robots;
std::vector<LeaderboardEntry> leaderboard;

bool global_paused = true;
int64_t global_game_time_ms = 300000; // Default 5 Min (Fallback)
int64_t last_loop_time = 0;

// Interrupt Variablen
volatile int pulse_count_buffer_1 = 0;
volatile int pulse_count_buffer_2 = 0;
volatile int64_t last_irq_time_1 = 0;
volatile int64_t last_irq_time_2 = 0;

void IRAM_ATTR gpio_isr_handler_1(void* arg) {
    int64_t now = esp_timer_get_time();
    if (now - last_irq_time_1 > 200000) { 
        pulse_count_buffer_1++;
        last_irq_time_1 = now;
    }
}
void IRAM_ATTR gpio_isr_handler_2(void* arg) {
    int64_t now = esp_timer_get_time();
    if (now - last_irq_time_2 > 200000) { 
        pulse_count_buffer_2++;
        last_irq_time_2 = now;
    }
}

void init_gpios() {
    gpio_reset_pin(PAUSE_LED_PIN);
    gpio_reset_pin(CONNECTION_LED_PIN);
    gpio_set_direction(PAUSE_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(CONNECTION_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PAUSE_LED_PIN, 1); 

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << ROBOT_PIN_1) | (1ULL << ROBOT_PIN_2);
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(ROBOT_PIN_1, gpio_isr_handler_1, (void*) ROBOT_PIN_1);
    gpio_isr_handler_add(ROBOT_PIN_2, gpio_isr_handler_2, (void*) ROBOT_PIN_2);
}

// Lokale Funktion zum Laden der Zeit (identisch zu webserver.cpp, aber hier im main scope nötig)
void load_startup_time() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        int64_t stored_time = 0;
        if (nvs_get_i64(my_handle, "game_time", &stored_time) == ESP_OK) {
            global_game_time_ms = stored_time;
            ESP_LOGI(TAG, "Startzeit aus NVS geladen: %lld ms", stored_time);
        }
        nvs_close(my_handle);
    } else {
        ESP_LOGW(TAG, "NVS konnte nicht geöffnet werden, nutze Default Zeit.");
    }
}

void init_master_robots() {
    if (!is_master) return;
    xSemaphoreTake(game_mutex, portMAX_DELAY);
    
    // Init mit der (potenziell aus NVS geladenen) Zeit
    Robot r1; r1.id = 1; strcpy(r1.name, "Master Left"); r1.pieces = 0; 
    r1.time_left_ms = global_game_time_ms; // <-- HIER
    r1.is_running = false; r1.client_ip = 0; r1.pin_index = 0; r1.last_seen = esp_timer_get_time(); r1.do_blink = false;
    robots.push_back(r1);

    Robot r2; r2.id = 2; strcpy(r2.name, "Master Right"); r2.pieces = 0; 
    r2.time_left_ms = global_game_time_ms; // <-- HIER
    r2.is_running = false; r2.client_ip = 0; r2.pin_index = 1; r2.last_seen = esp_timer_get_time(); r2.do_blink = false;
    robots.push_back(r2);
    
    xSemaphoreGive(game_mutex);
}

void send_to_master(int pin_idx, bool is_ping) {
    esp_http_client_config_t config = {};
    char url[128]; // Puffer vergrößert
    
    if (is_ping) {
        snprintf(url, sizeof(url), "http://192.168.4.1/api/ping?id=%lu", (unsigned long)my_client_id);
    } else {
        snprintf(url, sizeof(url), "http://192.168.4.1/api/pulse?pin=%d&id=%lu", pin_idx, (unsigned long)my_client_id);
    }
    
    config.url = url;
    config.timeout_ms = 1000;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    
    // (Optional: Dein Debug-Log von vorhin hier behalten wenn gewünscht)
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP Request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

void broadcast_all() {
    if(!is_master) return;

    cJSON *root = cJSON_CreateObject();
    xSemaphoreTake(game_mutex, portMAX_DELAY);
    
    cJSON_AddBoolToObject(root, "global_paused", global_paused);
    cJSON_AddNumberToObject(root, "global_time", (double)global_game_time_ms);

    cJSON *arr = cJSON_CreateArray();
    int64_t now = esp_timer_get_time();
    
    for(const auto &r : robots) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", r.id);
        cJSON_AddStringToObject(item, "name", r.name);
        cJSON_AddNumberToObject(item, "pieces", r.pieces);
        cJSON_AddNumberToObject(item, "time_left", (double)r.time_left_ms);
        cJSON_AddBoolToObject(item, "running", r.is_running);
        bool online = (now - r.last_seen) < 4000000;
        cJSON_AddBoolToObject(item, "online", online);
        
        char origin[32];
        if (r.client_ip == 0) strcpy(origin, "Master ESP");
        // NEU: Anzeige der ID statt IP
        else snprintf(origin, sizeof(origin), "Client ID: %04lX", (unsigned long)(r.client_ip & 0xFFFF)); 
        cJSON_AddStringToObject(item, "origin", origin);
        
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "robots", arr);

    cJSON *lb_arr = cJSON_CreateArray();
    for(const auto &entry : leaderboard) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", entry.team_name);
        cJSON_AddNumberToObject(e, "pieces", entry.moves);
        cJSON_AddItemToArray(lb_arr, e);
    }
    cJSON_AddItemToObject(root, "leaderboard", lb_arr);

    xSemaphoreGive(game_mutex);

    char *json_str = cJSON_PrintUnformatted(root);
    if(json_str) {
        ws_broadcast(json_str); 
        free(json_str);
    }
    cJSON_Delete(root);
}

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_flash_init();
    }
    
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA); 

    // FNV-1a Hash Algorithmus: Mixt alle 6 Bytes der MAC in einen 32-Bit Wert
    uint32_t hash = 2166136261u; // FNV Offset Basis
    for (int i = 0; i < 6; i++) {
        hash ^= mac[i];
        hash *= 16777619u;   // FNV Prime
    }
    my_client_id = hash;

    // Murphys Law: Auch ein Hash kann theoretisch 0 sein
    if (my_client_id == 0) my_client_id = 1;

    ESP_LOGI(TAG, "Client ID (Hash aus MAC): %lu", (unsigned long)my_client_id);
    
    load_startup_time();

    game_mutex = xSemaphoreCreateMutex();
    init_gpios();
    esp_netif_init();
    esp_event_loop_create_default();
    
    // ... Rest des Codes (setup_network etc.) ...
    setup_network();

    if (is_master) {
        init_master_robots(); 
        init_webserver(); 
        // init_udp_sync(); // Ist ja deaktiviert
    }
    
    // ... dein Loop ...
    last_loop_time = esp_timer_get_time();
    // ...
    // Hier musst du natürlich deinen Loop Code wieder einfügen oder den bestehenden lassen
    // Der Code oben ersetzt nur den Initialisierungsteil
    
    // Damit der Compiler nicht meckert, hier der Loop-Start Dummy (du hast ihn ja im Original):
    int64_t broadcast_timer = 0;
    int64_t ping_timer = 0;

    while(1) {
        // ... dein Loop Code 1:1 von vorher ...
        int64_t now = esp_timer_get_time();
        int64_t delta_us = now - last_loop_time;
        last_loop_time = now;
        int64_t delta_ms = delta_us / 1000;

        int p1 = pulse_count_buffer_1; pulse_count_buffer_1 = 0;
        int p2 = pulse_count_buffer_2; pulse_count_buffer_2 = 0;

        if (is_master) {
            xSemaphoreTake(game_mutex, portMAX_DELAY);
            
            if (!global_paused && global_game_time_ms > 0) {
                global_game_time_ms -= delta_ms;
                // ÄNDERUNG: Wenn 0 erreicht, global pausieren
                if(global_game_time_ms <= 0) {
                    global_game_time_ms = 0;
                    global_paused = true; 
                    ESP_LOGI(TAG, "Global Time abgelaufen -> Auto-Pause");
                }
            }

            for (auto &rob : robots) {
                if (!global_paused && rob.is_running && rob.time_left_ms > 0) {
                    rob.time_left_ms -= delta_ms;
                    // ÄNDERUNG: Wenn Roboter-Zeit 0 erreicht, Roboter stoppen
                    if(rob.time_left_ms <= 0) {
                        rob.time_left_ms = 0;
                        rob.is_running = false;
                        // Optional: Roboter-Namen loggen, falls vorhanden
                        ESP_LOGI(TAG, "Roboter ID %lu Zeit abgelaufen -> Stop", (unsigned long)rob.id);
                    }
                }
                
                // HIER WICHTIG: Die Logic prüft auf 0, aber wir nutzen ja jetzt IDs.
                // Da ID 0 "Master ESP" bedeutet (siehe broadcast_all Änderung vorhin),
                // müssen wir sicherstellen, dass wir beim Master die ID Logik sauber haben.
                // Wenn wir "is_master" sind, haben wir keine ID über URL bekommen, sondern sind lokal.
                
                // Deine Logik für Master-interne Roboter (Pin Index check):
                if (rob.client_ip == 0) { // Master Roboter hat id 0 
                    rob.last_seen = now; 
                    if (rob.is_running && !global_paused) { 
                        if (rob.pin_index == 0 && p1 > 0) rob.pieces += p1;
                        if (rob.pin_index == 1 && p2 > 0) rob.pieces += p2;
                    }
                }
            }
            xSemaphoreGive(game_mutex);

            broadcast_timer += delta_ms;
            if (broadcast_timer >= 200) {
                broadcast_all();
                broadcast_timer = 0;
            }
            gpio_set_level(PAUSE_LED_PIN, global_paused ? 1 : 0);

        } else {
            // Client Logic
            for (int i=0; i < p1; i++) send_to_master(0, false);
            for (int i=0; i < p2; i++) send_to_master(1, false);
            
            ping_timer += delta_ms;
            if (ping_timer >= 2000) { 
                send_to_master(0, true); 
                ping_timer = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
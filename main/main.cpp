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
#include <cstring>

static const char *TAG = "MAIN";

// Globale Variablen (Definitionen für die externs aus global_vars.h)
uint32_t my_client_id = 0;
SemaphoreHandle_t game_mutex = NULL;
std::vector<Robot> robots;
std::vector<LeaderboardEntry> leaderboard;

// HIER WURDE "is_master" ENTFERNT (kommt jetzt aus global_vars.h)

bool global_paused = true;
int64_t global_game_time_ms = 300000; // Default 5 Min
int64_t last_loop_time = 0;

// Interrupt Variablen
volatile int pulse_count_buffer_1 = 0;
volatile int pulse_count_buffer_2 = 0;
volatile int64_t last_irq_time_1 = 0;
volatile int64_t last_irq_time_2 = 0;

// --- ISR HANDLER ---
void IRAM_ATTR gpio_isr_handler_1(void* arg) {
    int64_t now = esp_timer_get_time();
    if (now - last_irq_time_1 > 200000) { 
        // Fix für volatile Warning: explizite Zuweisung
        pulse_count_buffer_1 = pulse_count_buffer_1 + 1;
        last_irq_time_1 = now;
    }
}
void IRAM_ATTR gpio_isr_handler_2(void* arg) {
    int64_t now = esp_timer_get_time();
    if (now - last_irq_time_2 > 200000) { 
        pulse_count_buffer_2 = pulse_count_buffer_2 + 1;
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

// --- ZEIT LADEN ---
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

// --- MASTER INTERNE ROBOTER ---
void init_master_robots() {
    if (!is_master) return;
    xSemaphoreTake(game_mutex, portMAX_DELAY);
    
    Robot r1; r1.id = 1; strcpy(r1.name, "Master Left"); r1.pieces = 0; 
    r1.time_left_ms = global_game_time_ms;
    r1.is_running = false; r1.client_ip = 0; r1.pin_index = 0; r1.last_seen = esp_timer_get_time(); r1.do_blink = false;
    robots.push_back(r1);

    Robot r2; r2.id = 2; strcpy(r2.name, "Master Right"); r2.pieces = 0; 
    r2.time_left_ms = global_game_time_ms;
    r2.is_running = false; r2.client_ip = 0; r2.pin_index = 1; r2.last_seen = esp_timer_get_time(); r2.do_blink = false;
    robots.push_back(r2);
    
    xSemaphoreGive(game_mutex);
}

// --- CLIENT SEND ---
void send_to_master(int pin_idx, bool is_ping) {
    esp_http_client_config_t config = {};
    char url[128];
    
    if (is_ping) {
        snprintf(url, sizeof(url), "http://192.168.4.1/api/ping?id=%lu", (unsigned long)my_client_id);
    } else {
        snprintf(url, sizeof(url), "http://192.168.4.1/api/pulse?pin=%d&id=%lu", pin_idx, (unsigned long)my_client_id);
    }
    
    config.url = url;
    config.timeout_ms = 1000;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    
    if (err != ESP_OK) {
        // Fehler nur spärlich loggen
    }
    esp_http_client_cleanup(client);
}

// --- BROADCAST (Sendet an WebSockets) ---
void broadcast_all() {
    if(!is_master) return;

    cJSON *root = cJSON_CreateObject();
    xSemaphoreTake(game_mutex, portMAX_DELAY);
    
    // Globale Infos
    cJSON_AddBoolToObject(root, "global_paused", global_paused);
    cJSON_AddNumberToObject(root, "global_time", (double)global_game_time_ms);

    // Roboter Liste
    cJSON *arr = cJSON_CreateArray();
    int64_t now = esp_timer_get_time();
    
    for(const auto &r : robots) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", r.id);
        cJSON_AddStringToObject(item, "name", r.name);
        cJSON_AddNumberToObject(item, "pieces", r.pieces);
        cJSON_AddNumberToObject(item, "time_left", (double)r.time_left_ms);
        cJSON_AddBoolToObject(item, "running", r.is_running);
        
        // Online Check (4 Sekunden Timeout)
        bool online = (now - r.last_seen) < 4000000;
        cJSON_AddBoolToObject(item, "online", online);
        
        // Herkunft anzeigen
        char origin[32];
        if (r.client_ip == 0) strcpy(origin, "Master ESP");
        else snprintf(origin, sizeof(origin), "Client ID: %lX", (unsigned long)r.client_ip); 
        cJSON_AddStringToObject(item, "origin", origin);
        
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "robots", arr);

    // Rangliste
    cJSON *lb_arr = cJSON_CreateArray();
    for(const auto &entry : leaderboard) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", entry.team_name);
        cJSON_AddNumberToObject(e, "pieces", entry.moves);
        cJSON_AddNumberToObject(e, "total_time", (double)entry.total_time_ms); 
        cJSON_AddItemToArray(lb_arr, e);
    }
    cJSON_AddItemToObject(root, "leaderboard", lb_arr);

    xSemaphoreGive(game_mutex);

    // Senden via Webserver-Helper
    char *json_str = cJSON_PrintUnformatted(root);
    if(json_str) {
        ws_broadcast(json_str); 
        free(json_str);
    }
    cJSON_Delete(root);
}

// --- MAIN ENTRY POINT ---
extern "C" void app_main(void) {
    // 1. NVS Initialisierung
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_flash_init();
    }
    
    // 2. Client ID generieren (Hash aus MAC)
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA); 
    uint32_t hash = 2166136261u; // FNV Offset Basis
    for (int i = 0; i < 6; i++) {
        hash ^= mac[i];
        hash *= 16777619u;
    }
    my_client_id = hash;
    if (my_client_id == 0) my_client_id = 1;

    ESP_LOGI(TAG, "Client ID (Hash): %lu", (unsigned long)my_client_id);
    
    // 3. Setup
    load_startup_time();
    
    load_leaderboard_nvs();

    game_mutex = xSemaphoreCreateMutex();
    init_gpios();
    esp_netif_init();
    esp_event_loop_create_default();
    
    // Netzwerk starten
    setup_network();

    if (is_master) {
        init_master_robots(); 
        init_webserver(); 
    }

    last_loop_time = esp_timer_get_time();
    int64_t broadcast_timer = 0;
    int64_t ping_timer = 0;

    // 4. Game Loop
    while(1) {
        int64_t now = esp_timer_get_time();
        int64_t delta_us = now - last_loop_time;
        last_loop_time = now;
        int64_t delta_ms = delta_us / 1000;

        int p1 = pulse_count_buffer_1; pulse_count_buffer_1 = 0;
        int p2 = pulse_count_buffer_2; pulse_count_buffer_2 = 0;

        if (is_master) {
            xSemaphoreTake(game_mutex, portMAX_DELAY);
            
            // Globaler Timer
            if (!global_paused && global_game_time_ms > 0) {
                global_game_time_ms -= delta_ms;
                if(global_game_time_ms <= 0) {
                    global_game_time_ms = 0;
                    global_paused = true; 
                    ESP_LOGI(TAG, "Global Time abgelaufen -> Auto-Pause");
                }
            }

            // Roboter Logic
            for (auto &rob : robots) {
                // Roboter Timer
                if (!global_paused && rob.is_running && rob.time_left_ms > 0) {
                    rob.time_left_ms -= delta_ms;
                    if(rob.time_left_ms <= 0) {
                        rob.time_left_ms = 0;
                        rob.is_running = false;
                        ESP_LOGI(TAG, "Roboter ID %lu Zeit abgelaufen -> Stop", (unsigned long)rob.id);
                    }
                }
                
                // Punkte zählen (nur für Master-eigene Roboter)
                if (rob.client_ip == 0) { 
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
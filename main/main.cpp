#include "webserver.h"
#include "config_wifi.h"
#include "global_vars.h"
#include "udp_sync.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_http_client.h" 
#include "cJSON.h"
#include <esp_netif.h>
#include <esp_event.h>

static const char *TAG = "MAIN";

SemaphoreHandle_t game_mutex = NULL;
std::vector<Robot> robots;
std::vector<LeaderboardEntry> leaderboard;

// Initialisierung der Globalen Zeit (z.B. 5 Minuten)
bool global_paused = true;
int64_t global_game_time_ms = 300000; 
int64_t last_loop_time = 0;

// Interrupt Variablen
volatile int pulse_count_buffer_1 = 0;
volatile int pulse_count_buffer_2 = 0;
volatile int64_t last_irq_time_1 = 0;
volatile int64_t last_irq_time_2 = 0;

// --- ISR (Bleibt gleich, funktioniert gut für High-Signale) ---
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

void init_master_robots() {
    if (!is_master) return;
    xSemaphoreTake(game_mutex, portMAX_DELAY);
    
    // Initialisiere mit individueller Zeit (optional gleich der globalen)
    Robot r1; r1.id = 1; strcpy(r1.name, "Master Left"); r1.pieces = 0; r1.time_left_ms = 300000; 
    r1.is_running = false; r1.client_ip = 0; r1.pin_index = 0; r1.last_seen = esp_timer_get_time(); r1.do_blink = false;
    robots.push_back(r1);

    Robot r2; r2.id = 2; strcpy(r2.name, "Master Right"); r2.pieces = 0; r2.time_left_ms = 300000; 
    r2.is_running = false; r2.client_ip = 0; r2.pin_index = 1; r2.last_seen = esp_timer_get_time(); r2.do_blink = false;
    robots.push_back(r2);
    
    xSemaphoreGive(game_mutex);
}

void send_to_master(int pin_idx, bool is_ping) {
    // Client ist dumm: Sendet nur Requests. Keine Zeitberechnung.
    esp_http_client_config_t config = {};
    char url[64];
    if (is_ping) snprintf(url, sizeof(url), "http://192.168.4.1/api/ping");
    else snprintf(url, sizeof(url), "http://192.168.4.1/api/pulse?pin=%d", pin_idx);
    
    config.url = url;
    config.timeout_ms = 500;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        if (!is_ping) {
            // LED Feedback für Puls
            gpio_set_level(CONNECTION_LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(CONNECTION_LED_PIN, 0);
        } else {
            // LED Feedback für Locate-Befehl vom Master
            int len = esp_http_client_get_content_length(client);
            if (len > 0) {
                char *buf = (char*)malloc(len + 1);
                int read_len = esp_http_client_read_response(client, buf, len);
                if (read_len > 0) {
                    buf[read_len] = '\0';
                    cJSON *root = cJSON_Parse(buf);
                    if (root) {
                        cJSON *blink = cJSON_GetObjectItem(root, "blink");
                        if (cJSON_IsTrue(blink)) {
                             for(int k=0; k<3; k++) {
                                gpio_set_level(CONNECTION_LED_PIN, 1); vTaskDelay(pdMS_TO_TICKS(200));
                                gpio_set_level(CONNECTION_LED_PIN, 0); vTaskDelay(pdMS_TO_TICKS(200));
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
                free(buf);
            }
        }
    }
    esp_http_client_cleanup(client);
}

void broadcast_all() {
    if(!is_master) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "global_paused", global_paused);
    
    // Jetzt die unabhängige globale Zeit senden
    xSemaphoreTake(game_mutex, portMAX_DELAY);
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
        
        bool online = (now - r.last_seen) < 4000000; // 4s Timeout
        cJSON_AddBoolToObject(item, "online", online);
        
        cJSON_AddItemToArray(arr, item);
    }
    xSemaphoreGive(game_mutex);
    cJSON_AddItemToObject(root, "robots", arr);

    char *json_str = cJSON_PrintUnformatted(root);
    if(json_str) {
        ws_broadcast(json_str); 
        free(json_str);
    }
    cJSON_Delete(root);
}

extern "C" void app_main(void) {
    nvs_flash_init();
    game_mutex = xSemaphoreCreateMutex();
    init_gpios();
    esp_netif_init();
    esp_event_loop_create_default();
    setup_network();

    if (is_master) {
        init_master_robots(); 
        init_webserver(); 
        init_udp_sync(); 
    }

    last_loop_time = esp_timer_get_time();
    int64_t broadcast_timer = 0;
    int64_t ping_timer = 0;

    while(1) {
        int64_t now = esp_timer_get_time();
        int64_t delta_us = now - last_loop_time;
        last_loop_time = now;
        int64_t delta_ms = delta_us / 1000;

        // Inputs abholen
        int p1 = pulse_count_buffer_1; pulse_count_buffer_1 = 0;
        int p2 = pulse_count_buffer_2; pulse_count_buffer_2 = 0;

        if (is_master) {
            // === MASTER ===
            xSemaphoreTake(game_mutex, portMAX_DELAY);
            
            // 1. Globale Zeit läuft unabhängig (nur bei Global Pause Stop)
            if (!global_paused && global_game_time_ms > 0) {
                global_game_time_ms -= delta_ms;
                if(global_game_time_ms < 0) global_game_time_ms = 0;
            }

            for (auto &rob : robots) {
                // 2. Individuelle Zeiten laufen NUR wenn Global AN UND Roboter AN
                if (!global_paused && rob.is_running && rob.time_left_ms > 0) {
                    rob.time_left_ms -= delta_ms;
                    if(rob.time_left_ms < 0) rob.time_left_ms = 0;
                }
                
                // Lokale Pulse verarbeiten
                if (rob.client_ip == 0) {
                    rob.last_seen = now; 
                    if (rob.is_running && !global_paused) { // Nur Zählen wenn Zeit läuft? (Oder immer?) -> User: "solange die zeit des jeweiligen roboters läuft"
                        if (rob.pin_index == 0 && p1 > 0) rob.pieces += p1;
                        if (rob.pin_index == 1 && p2 > 0) rob.pieces += p2;
                    }
                    
                    if (rob.do_blink) {
                        rob.do_blink = false; 
                        // Blinken...
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
            // === CLIENT (DUMMY) ===
            // Sendet stumpf Pulse, kümmert sich um nichts.
            for (int i=0; i < p1; i++) send_to_master(0, false);
            for (int i=0; i < p2; i++) send_to_master(1, false);
            
            ping_timer += delta_ms;
            if (ping_timer >= 2000) { // Alle 2s Ping reicht
                send_to_master(0, true); 
                ping_timer = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
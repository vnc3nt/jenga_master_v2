#include "webserver.h"
#include "global_vars.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <cJSON.h>
#include <lwip/sockets.h>
#include <esp_timer.h>
#include "nvs.h"
#include "nvs_flash.h"
#include <time.h>
#include <arpa/inet.h>

static const char *TAG = "WEB";
static httpd_handle_t server = NULL;

// --- NVS HELPER ---
int64_t load_time_from_nvs() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    int64_t time_ms = 300000; // Default 5min
    if (err == ESP_OK) {
        if (nvs_get_i64(my_handle, "game_time", &time_ms) != ESP_OK) time_ms = 300000; 
        nvs_close(my_handle);
    }
    return time_ms;
}

void save_time_to_nvs(int64_t time_ms) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_set_i64(my_handle, "game_time", time_ms);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

// --- NVS LEADERBOARD MANAGER ---
void save_leaderboard_nvs() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        // Wir speichern max 50 Einträge um NVS Überlauf zu verhindern
        size_t count = leaderboard.size();
        if (count > 50) count = 50; 
        
        // Da wir neuere Einträge hinten anfügen, nehmen wir die letzten 'count'
        size_t start_idx = leaderboard.size() - count;
        
        size_t required_size = count * sizeof(LeaderboardEntry);
        
        // Kopie für den Speicher erstellen (flat array)
        if (count > 0) {
            nvs_set_blob(my_handle, "lb_data", &leaderboard[start_idx], required_size);
        } else {
            nvs_erase_key(my_handle, "lb_data");
        }
        
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Leaderboard (%d Einträge) in NVS gespeichert.", (int)count);
    }
}

void load_leaderboard_nvs() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        size_t required_size = 0;
        if (nvs_get_blob(my_handle, "lb_data", NULL, &required_size) == ESP_OK && required_size > 0) {
            size_t count = required_size / sizeof(LeaderboardEntry);
            LeaderboardEntry* buf = (LeaderboardEntry*) malloc(required_size);
            
            if (buf) {
                if (nvs_get_blob(my_handle, "lb_data", buf, &required_size) == ESP_OK) {
                    leaderboard.clear();
                    for(size_t i=0; i<count; i++) {
                        leaderboard.push_back(buf[i]);
                    }
                    ESP_LOGI(TAG, "Leaderboard (%d Einträge) aus NVS geladen.", (int)count);
                }
                free(buf);
            }
        }
        nvs_close(my_handle);
    }
}

uint32_t get_client_id_from_url(const char* query) {
    char val[16];
    if (httpd_query_key_value(query, "id", val, sizeof(val)) == ESP_OK) {
        return (uint32_t)strtoul(val, NULL, 10);
    }
    return 0;
}

// --- WS BROADCAST ---
void ws_broadcast(const char* str) {
    if (!server) return;
    size_t fds = 7;
    int client_fds[7];
    if (httpd_get_client_list(server, &fds, client_fds) == ESP_OK) {
        for (size_t i = 0; i < fds; ++i) {
            if (httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_frame_t pkt;
                memset(&pkt, 0, sizeof(pkt));
                pkt.type = HTTPD_WS_TYPE_TEXT;
                pkt.payload = (uint8_t*)str;
                pkt.len = strlen(str);
                httpd_ws_send_frame_async(server, client_fds[i], &pkt);
            }
        }
    }
}

// --- HELPER: CREATE ROBOTS FOR CLIENT ---
void ensure_client_robots(uint32_t ip, int64_t now) {
    bool found = false;
    for(auto &r : robots) {
        if(r.client_ip == ip) {
            r.last_seen = now;
            found = true;
        }
    }
    
    if (!found) {
        ESP_LOGI(TAG, "Neuer Client erkannt (IP: %u). Erstelle Roboter...", (unsigned int)ip);
        
        Robot r1; r1.id = esp_random(); r1.client_ip = ip; r1.pin_index = 0;
        r1.pieces = 0;
        r1.time_left_ms = global_game_time_ms; // Startet mit aktueller Globalzeit
        r1.is_running = false; r1.last_seen = now; r1.do_blink = false;
        memset(r1.name, 0, sizeof(r1.name));
        
        Robot r2; r2.id = esp_random(); r2.client_ip = ip; r2.pin_index = 1;
        r2.pieces = 0;
        r2.time_left_ms = global_game_time_ms;
        r2.is_running = false; r2.last_seen = now; r2.do_blink = false;
        memset(r2.name, 0, sizeof(r2.name));
        
        robots.push_back(r1);
        robots.push_back(r2);
    }
}

// --- API HANDLER ---

// 1. PULSE (Punkte zählen)
esp_err_t api_pulse_handler(httpd_req_t *req) {
    char buf[128]; // Etwas größer für URL Parameter
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char val[4];
        if (httpd_query_key_value(buf, "pin", val, sizeof(val)) == ESP_OK) {
            int pin = atoi(val);
            
            // NEU: ID aus URL holen (statt IP Logik)
            uint32_t client_id = get_client_id_from_url(buf);
            if (client_id == 0) {
                 ESP_LOGW(TAG, "Pulse ohne Client ID ignoriert");
                 httpd_resp_send(req, "MISSING_ID", 10);
                 return ESP_OK;
            }

            xSemaphoreTake(game_mutex, portMAX_DELAY);
            int64_t now = esp_timer_get_time();
            
            // "client_id" nutzen statt "ip"
            ensure_client_robots(client_id, now);

            for(auto &r : robots) {
                if(r.client_ip == client_id && r.pin_index == pin) { // client_ip speichert jetzt die ID
                    if(r.is_running && !global_paused) {
                        r.pieces++;
                    }
                    r.last_seen = now;
                    break;
                }
            }
            xSemaphoreGive(game_mutex);
        }
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

// 2. PING (Heartbeat - Hier fehlte das Erstellen!)
esp_err_t api_ping_handler(httpd_req_t *req) {
    // URL Query String lesen
    char buf[128];
    uint32_t client_id = 0;
    
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        client_id = get_client_id_from_url(buf);
    }

    // Fallback falls keine ID (sollte nicht passieren mit neuem Client Code)
    if (client_id == 0) {
        ESP_LOGW(TAG, "Ping ohne ID empfangen!");
        // Wir könnten hier abbrechen, oder versuchen IP zu nehmen, 
        // aber das IP-Holen war ja der Bug. Also ignorieren wir es lieber.
        httpd_resp_send(req, "MISSING_ID", 10);
        return ESP_OK;
    }

    bool should_blink = false;

    xSemaphoreTake(game_mutex, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    
    // Roboter erstellen/updaten mit der ID
    ensure_client_robots(client_id, now);

    // Blink Check
    for(auto &r : robots) {
        if(r.client_ip == client_id) { // Nutzung von client_ip Feld für ID
            if (r.do_blink) {
                should_blink = true;
                r.do_blink = false; 
            }
        }
    }
    xSemaphoreGive(game_mutex);
    
    char resp_buf[64];
    snprintf(resp_buf, sizeof(resp_buf), "{\"blink\":%s}", should_blink ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_buf, strlen(resp_buf));
    return ESP_OK;
}

// --- STATIC FILES ---
static esp_err_t index_handler(httpd_req_t *req) {
    extern const unsigned char main_html_start[] asm("_binary_main_html_start");
    extern const unsigned char main_html_end[] asm("_binary_main_html_end");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)main_html_start, main_html_end - main_html_start);
}
static esp_err_t style_handler(httpd_req_t *req) {
    extern const unsigned char style_css_start[] asm("_binary_style_css_start");
    extern const unsigned char style_css_end[] asm("_binary_style_css_end");
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, (const char *)style_css_start, style_css_end - style_css_start);
}
static esp_err_t js_handler(httpd_req_t *req) {
    extern const unsigned char main_js_start[] asm("_binary_main_js_start");
    extern const unsigned char main_js_end[] asm("_binary_main_js_end");
    httpd_resp_set_type(req, "text/javascript");
    return httpd_resp_send(req, (const char *)main_js_start, main_js_end - main_js_start);
}

// --- WEBSOCKET ---
esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) return ESP_OK;

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    if (httpd_ws_recv_frame(req, &ws_pkt, 0) != ESP_OK) return ESP_FAIL;
    if (ws_pkt.len) {
        buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
        ws_pkt.payload = buf;
        if (httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len) != ESP_OK) { free(buf); return ESP_FAIL; }
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && buf) {
        cJSON *root = cJSON_Parse((char*)buf);
        if (root) {
            cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
            if (cJSON_IsString(cmd)) {
                
                if (strcmp(cmd->valuestring, "toggle_global") == 0) {
                    global_paused = !global_paused;
                    xSemaphoreTake(game_mutex, portMAX_DELAY);
                    for(auto &r : robots) r.is_running = !global_paused;
                    xSemaphoreGive(game_mutex);
                }
                else if (strcmp(cmd->valuestring, "set_all_time") == 0) {
                     cJSON *val = cJSON_GetObjectItem(root, "val");
                     if (cJSON_IsNumber(val)) {
                        xSemaphoreTake(game_mutex, portMAX_DELAY);
                        int64_t new_time = (int64_t)val->valuedouble * 1000;
                        global_game_time_ms = new_time;
                        save_time_to_nvs(new_time);
                        for(auto &r : robots) r.time_left_ms = new_time;
                        xSemaphoreGive(game_mutex);
                     }
                }
                else if (strcmp(cmd->valuestring, "reset_game") == 0) {
                    bool should_save = true;
                    cJSON *saveParam = cJSON_GetObjectItem(root, "save");
                    if (saveParam && cJSON_IsBool(saveParam)) should_save = cJSON_IsTrue(saveParam);

                    xSemaphoreTake(game_mutex, portMAX_DELAY);
                    if (should_save) {
                        int64_t now_ts = (int64_t)time(NULL); 
                        for(const auto &r : robots) {
                            if(r.pieces > 0 && strlen(r.name) > 0) {
                                LeaderboardEntry entry;
                                strncpy(entry.team_name, r.name, sizeof(entry.team_name)-1);
                                entry.team_name[31] = '\0';
                                entry.moves = r.pieces;
                                entry.time_ms = 0; 
                                entry.timestamp = now_ts;
                                entry.total_time_ms = global_game_time_ms; // NEU: Gesamtzeit speichern
                                leaderboard.push_back(entry);
                            }
                        }
                        save_leaderboard_nvs();
                    }
                    int64_t start_time = load_time_from_nvs();
                    global_game_time_ms = start_time;
                    global_paused = true;
                    for(auto &r : robots) {
                        r.pieces = 0; r.time_left_ms = start_time; r.is_running = false; r.do_blink = false;
                    }
                    xSemaphoreGive(game_mutex);
                }
                // Individual Controls
                else if (strcmp(cmd->valuestring, "toggle_robot") == 0) {
                    cJSON *id = cJSON_GetObjectItem(root, "id");
                    if (cJSON_IsNumber(id)) {
                        xSemaphoreTake(game_mutex, portMAX_DELAY);
                        for(auto &r : robots) { if(r.id == (uint32_t)id->valuedouble) { r.is_running = !r.is_running; break; } }
                        xSemaphoreGive(game_mutex);
                    }
                }
                else if (strcmp(cmd->valuestring, "adj_score") == 0) {
                     cJSON *id = cJSON_GetObjectItem(root, "id");
                     cJSON *val = cJSON_GetObjectItem(root, "val");
                     if (cJSON_IsNumber(id) && cJSON_IsNumber(val)) {
                        xSemaphoreTake(game_mutex, portMAX_DELAY);
                        for(auto &r : robots) { 
                            if(r.id == (uint32_t)id->valuedouble) { 
                                r.pieces += val->valueint; 
                                if(r.pieces<0) r.pieces=0; 
                                break; 
                            } 
                        }
                        xSemaphoreGive(game_mutex);
                     }
                }
                else if (strcmp(cmd->valuestring, "set_name") == 0) {
                    cJSON *id = cJSON_GetObjectItem(root, "id");
                    cJSON *val = cJSON_GetObjectItem(root, "val");
                    if (cJSON_IsNumber(id) && cJSON_IsString(val)) {
                        xSemaphoreTake(game_mutex, portMAX_DELAY);
                        for(auto &r : robots) { if(r.id == (uint32_t)id->valuedouble) { strncpy(r.name, val->valuestring, sizeof(r.name)-1); r.name[sizeof(r.name)-1] = '\0'; break; } }
                        xSemaphoreGive(game_mutex);
                    }
                }
                else if (strcmp(cmd->valuestring, "locate") == 0) {
                    cJSON *id = cJSON_GetObjectItem(root, "id");
                    if(cJSON_IsNumber(id)) {
                        xSemaphoreTake(game_mutex, portMAX_DELAY);
                        for(auto &r : robots) { if(r.id == (uint32_t)id->valuedouble) { r.do_blink = true; break; } }
                        xSemaphoreGive(game_mutex);
                    }
                }
                else if (strcmp(cmd->valuestring, "delete_all") == 0) {
                    xSemaphoreTake(game_mutex, portMAX_DELAY);
                    leaderboard.clear();
                    xSemaphoreGive(game_mutex);
                }
            }
            cJSON_Delete(root);
        }
    }
    free(buf);
    return ESP_OK;
}

// Leerer Handler für das Favicon, um 404-Warnungen zu vermeiden
esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0); // Leere Antwort
    return ESP_OK;
}

void init_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.max_open_sockets = 7;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t u_idx = { "/", HTTP_GET, index_handler, NULL }; httpd_register_uri_handler(server, &u_idx);
        httpd_uri_t u_css = { "/style.css", HTTP_GET, style_handler, NULL }; httpd_register_uri_handler(server, &u_css);
        httpd_uri_t u_js = { "/main.js", HTTP_GET, js_handler, NULL }; httpd_register_uri_handler(server, &u_js);
        httpd_uri_t u_pulse = { "/api/pulse", HTTP_GET, api_pulse_handler, NULL }; httpd_register_uri_handler(server, &u_pulse);
        httpd_uri_t u_ping = { "/api/ping", HTTP_GET, api_ping_handler, NULL }; httpd_register_uri_handler(server, &u_ping);
        httpd_uri_t u_ws = { "/ws", HTTP_GET, ws_handler, NULL };
        httpd_uri_t u_favicon = { "/favicon.ico", HTTP_GET, favicon_handler, NULL }; httpd_register_uri_handler(server, &u_favicon);
        u_ws.is_websocket = true; u_ws.handle_ws_control_frames = true; httpd_register_uri_handler(server, &u_ws);
        ESP_LOGI(TAG, "Webserver gestartet");
    }
}
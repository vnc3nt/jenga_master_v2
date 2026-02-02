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

static const char *TAG = "WEB";
static httpd_handle_t server = NULL;

// --- NVS HELPER ---
int64_t load_time_from_nvs() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    int64_t time_ms = 300000;
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

// --- BROADCAST ---
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

// --- HANDLERS ---
esp_err_t api_pulse_handler(httpd_req_t *req) {
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char val[4];
        if (httpd_query_key_value(buf, "pin", val, sizeof(val)) == ESP_OK) {
            int pin = atoi(val);
            int sockfd = httpd_req_to_sockfd(req);
            struct sockaddr_in addr;
            socklen_t addr_len = sizeof(addr);
            getpeername(sockfd, (struct sockaddr *)&addr, &addr_len);
            uint32_t ip = addr.sin_addr.s_addr;

            xSemaphoreTake(game_mutex, portMAX_DELAY);
            bool found = false;
            int64_t now = esp_timer_get_time();
            
            for(auto &r : robots) {
                if(r.client_ip == ip && r.pin_index == pin) {
                    if(r.is_running && !global_paused) r.pieces++;
                    r.last_seen = now; found = true; break;
                }
            }
            if (!found) {
                Robot r1; r1.id = esp_random(); r1.client_ip = ip; r1.pin_index = 0;
                r1.pieces = 0; r1.time_left_ms = global_game_time_ms; r1.is_running = false; r1.last_seen = now; r1.do_blink = false; memset(r1.name, 0, 32);
                Robot r2; r2.id = esp_random(); r2.client_ip = ip; r2.pin_index = 1;
                r2.pieces = 0; r2.time_left_ms = global_game_time_ms; r2.is_running = false; r2.last_seen = now; r2.do_blink = false; memset(r2.name, 0, 32);
                robots.push_back(r1); robots.push_back(r2);
            }
            xSemaphoreGive(game_mutex);
        }
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

esp_err_t api_ping_handler(httpd_req_t *req) {
    int sockfd = httpd_req_to_sockfd(req);
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    getpeername(sockfd, (struct sockaddr *)&addr, &addr_len);
    uint32_t ip = addr.sin_addr.s_addr;
    bool should_blink = false;

    xSemaphoreTake(game_mutex, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    for(auto &r : robots) {
        if(r.client_ip == ip) {
            r.last_seen = now;
            if (r.do_blink) { should_blink = true; r.do_blink = false; }
        }
    }
    xSemaphoreGive(game_mutex);
    char resp[32]; snprintf(resp, 32, "{\"blink\":%s}", should_blink?"true":"false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t idx_h(httpd_req_t *req) { extern const unsigned char main_html_start[] asm("_binary_main_html_start"); extern const unsigned char main_html_end[] asm("_binary_main_html_end"); httpd_resp_send(req, (const char *)main_html_start, main_html_end - main_html_start); return ESP_OK; }
static esp_err_t css_h(httpd_req_t *req) { extern const unsigned char style_css_start[] asm("_binary_style_css_start"); extern const unsigned char style_css_end[] asm("_binary_style_css_end"); httpd_resp_set_type(req, "text/css"); httpd_resp_send(req, (const char *)style_css_start, style_css_end - style_css_start); return ESP_OK; }
static esp_err_t js_h(httpd_req_t *req) { extern const unsigned char main_js_start[] asm("_binary_main_js_start"); extern const unsigned char main_js_end[] asm("_binary_main_js_end"); httpd_resp_set_type(req, "text/javascript"); httpd_resp_send(req, (const char *)main_js_start, main_js_end - main_js_start); return ESP_OK; }

esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) return ESP_OK;
    httpd_ws_frame_t ws_pkt; uint8_t *buf = NULL; memset(&ws_pkt, 0, sizeof(ws_pkt));
    if (httpd_ws_recv_frame(req, &ws_pkt, 0) != ESP_OK) return ESP_FAIL;
    if (ws_pkt.len) {
        buf = (uint8_t *)calloc(1, ws_pkt.len + 1); ws_pkt.payload = buf;
        if (httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len) != ESP_OK) { free(buf); return ESP_FAIL; }
    }
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && buf) {
        cJSON *root = cJSON_Parse((char*)buf);
        if (root) {
            cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
            if (cJSON_IsString(cmd)) {
                
                // GLOBAL PAUSE
                if (strcmp(cmd->valuestring, "toggle_global") == 0) {
                    ESP_LOGI(TAG, "CMD: Toggle Global");
                    xSemaphoreTake(game_mutex, portMAX_DELAY);
                    // Verhindern dass bei Zeit 0 gestartet wird
                    if (global_game_time_ms > 0 || !global_paused) { 
                        global_paused = !global_paused;
                        for(auto &r : robots) r.is_running = !global_paused;
                    }
                    xSemaphoreGive(game_mutex);
                }
                
                // RESET GAME
                else if (strcmp(cmd->valuestring, "reset_game") == 0) {
                    ESP_LOGI(TAG, "CMD: Reset Game");
                    bool should_save = false;
                    cJSON *s = cJSON_GetObjectItem(root, "save");
                    if(s && cJSON_IsBool(s)) should_save = cJSON_IsTrue(s);

                    xSemaphoreTake(game_mutex, portMAX_DELAY);
                    if (should_save) {
                        int64_t ts = (int64_t)time(NULL);
                        for(const auto &r : robots) {
                            if(r.pieces > 0 && strlen(r.name)>0) {
                                LeaderboardEntry e; strncpy(e.team_name, r.name, 31); e.team_name[31]=0;
                                e.moves = r.pieces; e.time_ms = 0; e.timestamp = ts;
                                leaderboard.push_back(e);
                            }
                        }
                    }
                    int64_t start_time = load_time_from_nvs();
                    global_game_time_ms = start_time;
                    global_paused = true;
                    for(auto &r : robots) { r.pieces = 0; r.time_left_ms = start_time; r.is_running = false; r.do_blink = false; }
                    xSemaphoreGive(game_mutex);
                }

                // SET TIME
                else if (strcmp(cmd->valuestring, "set_all_time") == 0) {
                     cJSON *val = cJSON_GetObjectItem(root, "val");
                     if (cJSON_IsNumber(val)) {
                        xSemaphoreTake(game_mutex, portMAX_DELAY);
                        int64_t t = (int64_t)val->valuedouble * 1000;
                        global_game_time_ms = t; save_time_to_nvs(t);
                        for(auto &r : robots) r.time_left_ms = t;
                        xSemaphoreGive(game_mutex);
                     }
                }
                // OTHER
                else if (strcmp(cmd->valuestring, "toggle_robot") == 0) {
                    cJSON *id = cJSON_GetObjectItem(root, "id");
                    if(cJSON_IsNumber(id)) {
                        xSemaphoreTake(game_mutex, portMAX_DELAY);
                        for(auto &r:robots) { if(r.id==(uint32_t)id->valuedouble) { r.is_running = !r.is_running; break; }}
                        xSemaphoreGive(game_mutex);
                    }
                }
                else if (strcmp(cmd->valuestring, "adj_score") == 0) {
                     cJSON *id = cJSON_GetObjectItem(root, "id"); cJSON *v = cJSON_GetObjectItem(root, "val");
                     if(cJSON_IsNumber(id) && cJSON_IsNumber(v)) {
                        xSemaphoreTake(game_mutex, portMAX_DELAY);
                        for(auto &r:robots) { if(r.id==(uint32_t)id->valuedouble) { r.pieces += v->valueint; if(r.pieces<0)r.pieces=0; break; }}
                        xSemaphoreGive(game_mutex);
                     }
                }
                else if (strcmp(cmd->valuestring, "set_name") == 0) {
                    cJSON *id = cJSON_GetObjectItem(root, "id"); cJSON *v = cJSON_GetObjectItem(root, "val");
                    if(cJSON_IsNumber(id) && cJSON_IsString(v)) {
                        xSemaphoreTake(game_mutex, portMAX_DELAY);
                        for(auto &r:robots) { if(r.id==(uint32_t)id->valuedouble) { strncpy(r.name, v->valuestring, 31); r.name[31]=0; break; }}
                        xSemaphoreGive(game_mutex);
                    }
                }
                else if (strcmp(cmd->valuestring, "locate") == 0) {
                    cJSON *id = cJSON_GetObjectItem(root, "id");
                    if(cJSON_IsNumber(id)) {
                        xSemaphoreTake(game_mutex, portMAX_DELAY);
                        for(auto &r:robots) { if(r.id==(uint32_t)id->valuedouble) { r.do_blink=true; break; }}
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
    free(buf); return ESP_OK;
}

void init_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12; config.max_open_sockets = 7;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t u_idx = { "/", HTTP_GET, idx_h, NULL }; httpd_register_uri_handler(server, &u_idx);
        httpd_uri_t u_css = { "/style.css", HTTP_GET, css_h, NULL }; httpd_register_uri_handler(server, &u_css);
        httpd_uri_t u_js = { "/main.js", HTTP_GET, js_h, NULL }; httpd_register_uri_handler(server, &u_js);
        httpd_uri_t u_p = { "/api/pulse", HTTP_GET, api_pulse_handler, NULL }; httpd_register_uri_handler(server, &u_p);
        httpd_uri_t u_pi = { "/api/ping", HTTP_GET, api_ping_handler, NULL }; httpd_register_uri_handler(server, &u_pi);
        httpd_uri_t u_ws = { "/ws", HTTP_GET, ws_handler, NULL }; u_ws.is_websocket = true; u_ws.handle_ws_control_frames = true;
        httpd_register_uri_handler(server, &u_ws);
        ESP_LOGI(TAG, "Webserver gestartet");
    }
}
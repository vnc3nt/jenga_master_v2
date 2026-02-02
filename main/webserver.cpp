#include "webserver.h"
#include <esp_event.h>
#include "driver/gpio.h"
#include <esp_log.h>
#include "config_wifi.h" // Include for Config Handlers
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_wifi.h"
#include "protocol_examples_common.h"
#include "lwip/sockets.h"
#include <esp_http_server.h>
#include "keep_alive.h"
#include "sdkconfig.h"
#include "mdns.h"
#include "global_vars.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wss_server";

static bool mdns_hostname_in_use(const char *hostname, uint32_t timeout_ms)
{
    esp_ip4_addr_t addr4 = {};
    esp_ip6_addr_t addr6 = {};

    // Falls im Netz bereits ein Host mit diesem Namen existiert, beantwortet er die mDNS A/AAAA Query.
    if (mdns_query_a(hostname, timeout_ms, &addr4) == ESP_OK) {
        return true;
    }
    if (mdns_query_aaaa(hostname, timeout_ms, &addr6) == ESP_OK) {
        return true;
    }
    return false;
}

static void mdns_select_hostname(char *out_hostname, size_t out_len)
{
    // 1. Zuerst im NVS nach fester Konfiguration suchen
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        int32_t val = 0;
        bool found = false;
        
        // Priorität: Neuer numerischer Key "jenga_id"
        if (nvs_get_i32(my_handle, "jenga_id", &val) == ESP_OK) {
            found = true;
        } 
        // Fallback: Alter String Key "mdns_id"
        else {
            char buf[16] = {0};
            size_t s = sizeof(buf);
            if (nvs_get_str(my_handle, "mdns_id", buf, &s) == ESP_OK) {
                val = atoi(buf);
                if (val > 0) found = true;
            }
        }
        nvs_close(my_handle);

        if (found && val >= 1 && val <= 99) {
            snprintf(out_hostname, out_len, "jenga%d", (int)val);
            ESP_LOGI(TAG, "Verwende feste Geräte-ID aus NVS: %s", out_hostname);
            return;
        }
    }

    // 2. Keine ID gefunden -> Auto-Scan (jenga1..99)
    // Vorgabe: jenga1.local, jenga2.local, ...
    // Wir speichern ohne ".local" (mdns_hostname_set hängt .local implizit an).
    static constexpr uint32_t kProbeTimeoutMs = 250;
    static constexpr int kMaxIndex = 99;

    for (int i = 1; i <= kMaxIndex; ++i) {
        char candidate[32];
        snprintf(candidate, sizeof(candidate), "jenga%d", i);
        if (!mdns_hostname_in_use(candidate, kProbeTimeoutMs)) {
            strlcpy(out_hostname, candidate, out_len);
            return;
        }
    }

    // Sehr unwahrscheinlich – aber falls 1..99 belegt sind.
    strlcpy(out_hostname, "jenga99", out_len);
}

// --- mDNS Implementierung ---
void initialise_mdns(void)
{
    ESP_LOGI(TAG, "Initialisiere mDNS...");
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MDNS Init fehlgeschlagen: %d", err);
        return;
    }

    char hostname[32];
    mdns_select_hostname(hostname, sizeof(hostname));

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS Hostname setzen fehlgeschlagen: %d", err);
        return;
    }

    mdns_instance_name_set("Jenga ESP32 Webserver");

    // Service bekanntgeben (optional, hilft aber Discovery-Tools)
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI(TAG, "mDNS gestartet. Erreichbar unter: http://%s.local", hostname);
}
// ---------------------------

// --- EXTERNE API FUNKTIONEN (aus main.cpp) ---
// Damit wir von hier aus die Spiellogik steuern können
extern void api_set_paused(bool paused);
extern void api_set_countdown_manual(uint32_t new_time_ms);
extern void api_increment_piece_counter();
extern void api_decrement_piece_counter();
extern void api_set_game_mode(int mode);
extern bool api_get_is_paused(); 
// NEU:
extern void api_reset_time();
extern void api_save_and_reset(const char* team_name, bool tower_fell);
extern void api_set_team_name(const char* team_name);

// --- API FUNKTIONEN FÜR LEADERBOARD EDIT ---
extern void api_update_entry(uint32_t id, const char* team, int moves, int64_t time, bool fell);
extern void api_delete_entry(uint32_t id);
extern void api_clear_leaderboard();
// LOCATE
extern void api_trigger_locate();

// --- WEBSERVER & WEBSOCKET ---
static httpd_handle_t server = NULL;

#if !CONFIG_HTTPD_WS_SUPPORT
#error This example cannot be used unless HTTPD_WS_SUPPORT is enabled in esp-http-server component configuration
#endif

struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
};

// WebSocket-Clients (Keep-Alive überwacht nur diese)
static const size_t max_ws_clients = 3;
// HTTPD-Sockets (Browser macht mehrere parallele HTTP Fetches + WS)
static const size_t max_open_sockets = 7;

// --- BROADCAST FUNKTION (Wird von main.cpp aufgerufen) ---
void ws_broadcast(const char* str) {
    if (server == NULL) return;
    
    size_t clients = max_open_sockets;
    int client_fds[max_open_sockets];
    
    // Liste aller verbundenen Clients abrufen
    if (httpd_get_client_list(server, &clients, client_fds) == ESP_OK) {
        for (size_t i = 0; i < clients; ++i) {
            int sock = client_fds[i];
            
            // Prüfen, ob es ein WebSocket Client ist
            if (httpd_ws_get_fd_info(server, sock) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_frame_t ws_pkt;
                memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
                ws_pkt.final = true;
                ws_pkt.fragmented = false;
                ws_pkt.type = HTTPD_WS_TYPE_TEXT;
                ws_pkt.payload = (uint8_t*)str;
                ws_pkt.len = strlen(str);
                
                // Asynchron senden, um den Main-Loop nicht zu blockieren
                httpd_ws_send_frame_async(server, sock, &ws_pkt);
            }
        }
    }
}

// --- WEBSOCKET HANDLER (Empfängt Daten von JS) ---
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, new connection opened");

        // Ab hier ist es ein echter WebSocket-Client: erst jetzt in Keep-Alive aufnehmen.
        {
            wss_keep_alive_t h = (wss_keep_alive_t)httpd_get_global_user_ctx(req->handle);
            if (h != NULL) {
                int fd = httpd_req_to_sockfd(req);
                (void)wss_keep_alive_add_client(h, fd);
                (void)wss_keep_alive_client_is_active(h, fd);
            }
        }

        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    // 1. Länge abrufen
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    
    // 2. Speicher reservieren und Daten lesen
    if (ws_pkt.len) {
        buf = (uint8_t*)calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
    }

    // Eingehende WS-Frames zählen als Aktivität (wichtig für Keep-Alive / Reconnect)
    {
        wss_keep_alive_t h = (wss_keep_alive_t)httpd_get_global_user_ctx(req->handle);
        if (h != NULL) {
            int fd = httpd_req_to_sockfd(req);
            esp_err_t ar = wss_keep_alive_client_is_active(h, fd);
            if (ar == ESP_ERR_NOT_FOUND) {
                (void)wss_keep_alive_add_client(h, fd);
                (void)wss_keep_alive_client_is_active(h, fd);
            }
        }
    }

    // 3. Daten verarbeiten
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        ESP_LOGI(TAG, "Received packet: %s", ws_pkt.payload);
        
        // JSON Parsen
        cJSON *root = cJSON_Parse((char*)ws_pkt.payload);
        if (root != NULL) {
            cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
            
            if (cJSON_IsString(cmd)) {
                // --- BEFEHLSVERARBEITUNG ---
                
                if (strcmp(cmd->valuestring, "toggle_pause") == 0) {
                    bool current = api_get_is_paused();
                    api_set_paused(!current);
                }
                else if (strcmp(cmd->valuestring, "set_mode") == 0) {
                    cJSON *val = cJSON_GetObjectItem(root, "val");
                    if (cJSON_IsNumber(val)) {
                        api_set_game_mode(val->valueint);
                    }
                }
                else if (strcmp(cmd->valuestring, "inc_moves") == 0) {
                    api_increment_piece_counter();
                }
                else if (strcmp(cmd->valuestring, "dec_moves") == 0) {
                    api_decrement_piece_counter();
                }
                else if (strcmp(cmd->valuestring, "set_time") == 0) {
                    cJSON *val = cJSON_GetObjectItem(root, "val");
                    if (cJSON_IsNumber(val)) {
                        api_set_countdown_manual((uint32_t)val->valueint);
                    }
                }
                // NEU: Reset Command
                else if (strcmp(cmd->valuestring, "reset_time") == 0) {
                    api_reset_time();
                }
                // NEU: Save and Reset Command
                else if (strcmp(cmd->valuestring, "save_and_reset") == 0) {
                    cJSON *team = cJSON_GetObjectItem(root, "team");
                    cJSON *fell = cJSON_GetObjectItem(root, "fell");
                    
                    const char* teamName = (cJSON_IsString(team)) ? team->valuestring : "Unbekannt";
                    bool fellBool = (cJSON_IsBool(fell)) ? cJSON_IsTrue(fell) : false;
                    
                    api_save_and_reset(teamName, fellBool);
                }
                // NEU: Set Team Name (für Start Check)
                else if (strcmp(cmd->valuestring, "set_team_name") == 0) {
                    cJSON *team = cJSON_GetObjectItem(root, "team");
                    const char* teamName = (cJSON_IsString(team)) ? team->valuestring : "";
                    api_set_team_name(teamName);
                }
                // NEU: Update Entry Command
                else if (strcmp(cmd->valuestring, "update_entry") == 0) {
                    ESP_LOGI(TAG, "Received update_entry command");
                    // id, team, moves, time, fell
                    cJSON *id = cJSON_GetObjectItem(root, "id");
                    cJSON *team = cJSON_GetObjectItem(root, "team");
                    cJSON *moves = cJSON_GetObjectItem(root, "moves");
                    cJSON *timeVal = cJSON_GetObjectItem(root, "time");
                    cJSON *fell = cJSON_GetObjectItem(root, "fell");
                    
                    if (cJSON_IsNumber(id) && cJSON_IsString(team) && cJSON_IsNumber(moves) && cJSON_IsNumber(timeVal)) {
                        bool fellBool = cJSON_IsTrue(fell);
                        // Use valuedouble for ID to avoid overflow issues with uint32_t > INT_MAX
                        api_update_entry((uint32_t)id->valuedouble, team->valuestring, moves->valueint, (int64_t)timeVal->valuedouble, fellBool);
                    } else {
                        ESP_LOGE(TAG, "Invalid params for update_entry: ID=%d Team=%d Moves=%d Time=%d",
                                 cJSON_IsNumber(id), cJSON_IsString(team), cJSON_IsNumber(moves), cJSON_IsNumber(timeVal));
                    }
                }
                // NEU: Delete Entry Command
                else if (strcmp(cmd->valuestring, "delete_entry") == 0) {
                    cJSON *id = cJSON_GetObjectItem(root, "id");
                    if (cJSON_IsNumber(id)) {
                         // Use valuedouble for ID
                        api_delete_entry((uint32_t)id->valuedouble);
                    }
                }
                // NEU: Clear Leaderboard Command
                else if (strcmp(cmd->valuestring, "clear_leaderboard") == 0) {
                    api_clear_leaderboard();
                }
                else if (strcmp(cmd->valuestring, "locate_device") == 0) {
                    api_trigger_locate();
                }
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "Invalid JSON received");
        }
    } 
    else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        // Ping -> Pong
        ws_pkt.type = HTTPD_WS_TYPE_PONG;
        httpd_ws_send_frame(req, &ws_pkt);
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        // PONG ist die Antwort auf unser PING (Keep-Alive).
        // Ohne dieses Update würde der Client nach wenigen Sekunden als "not alive" gelten.
        wss_keep_alive_t h = (wss_keep_alive_t)httpd_get_global_user_ctx(req->handle);
        if (h != NULL) {
            int fd = httpd_req_to_sockfd(req);
            (void)wss_keep_alive_client_is_active(h, fd);
        }
    }

    free(buf);
    return ESP_OK;
}

esp_err_t wss_open_fd(httpd_handle_t hd, int sockfd)
{
    ESP_LOGI(TAG, "New client connected %d", sockfd);
    // NICHT hier in Keep-Alive aufnehmen: open_fn wird auch für normale HTTP Requests aufgerufen.
    // WS-Clients werden im WS-Handshake (ws_handler, HTTP_GET) registriert.
    return ESP_OK;
}

void wss_close_fd(httpd_handle_t hd, int sockfd)
{
    ESP_LOGI(TAG, "Client disconnected %d", sockfd);
    
    // 1. Aus Keep-Alive Liste entfernen
    wss_keep_alive_t h = (wss_keep_alive_t)httpd_get_global_user_ctx(hd);
    if (h) {
        wss_keep_alive_remove_client(h, sockfd);
    }

    // 2. "Kill it with fire" - Taktik:
    // Wir sagen dem TCP-Stack, er soll nicht warten (kein TIME_WAIT),
    // sondern die Verbindung sofort hart beenden (RST).
    struct linger l;
    l.l_onoff = 1;
    l.l_linger = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));

    // 3. WICHTIG: Den Socket tatsächlich schließen!
    close(sockfd); 
}

static const httpd_uri_t ws = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = ws_handler,
        .user_ctx   = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true,
        .supported_subprotocol = NULL
};

// --- ENTFERNT: send_hello (wurde nicht genutzt und verursachte Warnung) ---

static void send_ping(void *arg)
{
    struct async_resp_arg *resp_arg = (struct async_resp_arg *)arg;
    httpd_handle_t hd = resp_arg->hd;
    int fd = resp_arg->fd;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = NULL;
    ws_pkt.len = 0;
    ws_pkt.type = HTTPD_WS_TYPE_PING;

    httpd_ws_send_frame_async(hd, fd, &ws_pkt);
    free(resp_arg);
}

bool client_not_alive_cb(wss_keep_alive_t h, int fd)
{
    ESP_LOGE(TAG, "Client not alive, closing fd %d", fd);
    httpd_sess_trigger_close(wss_keep_alive_get_user_ctx(h), fd);
    return true;
}

bool check_client_alive_cb(wss_keep_alive_t h, int fd)
{
    struct async_resp_arg *resp_arg = (struct async_resp_arg *)malloc(sizeof(struct async_resp_arg));
    assert(resp_arg != NULL);
    resp_arg->hd = (httpd_handle_t)wss_keep_alive_get_user_ctx(h);
    resp_arg->fd = fd;

    if (httpd_queue_work(resp_arg->hd, send_ping, resp_arg) == ESP_OK) {
        return true;
    }

    free(resp_arg);
    return false;
}

// --- FILE HANDLERS ---
static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    extern const unsigned char main_html_start[] asm("_binary_main_html_start");
    extern const unsigned char main_html_end[] asm("_binary_main_html_end");
    const size_t main_html_size = (main_html_end - main_html_start);
    return httpd_resp_send(req, (const char *)main_html_start, main_html_size);
}

static esp_err_t style_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Serving style.css"); // <--- DIESE ZEILE HINZUFÜGEN
    httpd_resp_set_type(req, "text/css");
    // Cache für 1 Jahr (31536000 Sekunden)
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000"); 

    extern const unsigned char style_css_start[] asm("_binary_style_css_start");
    extern const unsigned char style_css_end[] asm("_binary_style_css_end");
    const size_t style_css_size = (style_css_end - style_css_start);
    return httpd_resp_send(req, (const char *)style_css_start, style_css_size);
}

static esp_err_t main_js_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/javascript");
    // Cache für 1 Jahr
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");

    extern const unsigned char main_js_start[] asm("_binary_main_js_start");
    extern const unsigned char main_js_end[] asm("_binary_main_js_end");
    const size_t main_js_size = (main_js_end - main_js_start);
    return httpd_resp_send(req, (const char *)main_js_start, main_js_size);
}

static esp_err_t sun_moon_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/png");
    // Cache für 1 Jahr
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");

    extern const unsigned char img_sun_moon_png_start[] asm("_binary_sun_moon_png_start");
    extern const unsigned char img_sun_moon_png_end[] asm("_binary_sun_moon_png_end");
    const size_t img_sun_moon_png_size = (img_sun_moon_png_end - img_sun_moon_png_start);
    return httpd_resp_send(req, (const char *)img_sun_moon_png_start, img_sun_moon_png_size);
}

// --- API HANDLERS (HIERHIN VERSCHOBEN) ---
static esp_err_t api_state_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    
    // Platzhalter-Werte (bitte später mit echten globalen Variablen ersetzen)
    cJSON_AddStringToObject(root, "mode", "A");
    cJSON_AddNumberToObject(root, "time", 60);
    cJSON_AddNumberToObject(root, "moves", 0);
    cJSON_AddBoolToObject(root, "running", false);

    const char *json_response = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, strlen(json_response));
    
    free((void*)json_response);
    cJSON_Delete(root);
    return ESP_OK;
}

// Favicon Handler (um 404 Fehler im Log zu vermeiden)
static esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}


// --- SERVER START ---
static httpd_handle_t start_wss_echo_server(void)
{
    wss_keep_alive_config_t keep_alive_config = KEEP_ALIVE_CONFIG_DEFAULT();
    keep_alive_config.max_clients = max_ws_clients;
    keep_alive_config.client_not_alive_cb = client_not_alive_cb;
    keep_alive_config.check_client_alive_cb = check_client_alive_cb;
    wss_keep_alive_t keep_alive = wss_keep_alive_start(&keep_alive_config);

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 7; // Set to 7 (default) to allow 3 clients + overhead, but trigger purge sooner.
    config.lru_purge_enable = true;
    // Browser-Refresh-Spam: HTTP Keep-Alive aus -> Sockets werden schneller frei.
    config.keep_alive_enable = false;
    
    config.global_user_ctx = keep_alive;
    config.open_fn = wss_open_fd;
    config.close_fn = wss_close_fd;
    
    config.max_uri_handlers = 20; // Increased for Config Handlers

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGI(TAG, "Error starting server!");
        return NULL;
    }

    wss_keep_alive_set_user_ctx(keep_alive, server);

    // URIs registrieren
    httpd_register_uri_handler(server, &ws);

    httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t style_uri = { .uri = "/style.css", .method = HTTP_GET, .handler = style_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &style_uri);

    httpd_uri_t script_uri = { .uri = "/main.js", .method = HTTP_GET, .handler = main_js_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &script_uri);

    httpd_uri_t sun_moon_uri = { .uri = "/img/sun-moon.png", .method = HTTP_GET, .handler = sun_moon_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &sun_moon_uri);

    // --- NEU: API & Favicon registrieren ---
    httpd_uri_t api_state_uri = { .uri = "/api/state", .method = HTTP_GET, .handler = api_state_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &api_state_uri);

    httpd_uri_t favicon_uri = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &favicon_uri);

    // --- Config Page Handlers (Imported from config_wifi) ---
    httpd_uri_t config_html_uri = { .uri = "/config.html", .method = HTTP_GET, .handler = config_html_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &config_html_uri);

    httpd_uri_t config_alias_uri = { .uri = "/config", .method = HTTP_GET, .handler = config_html_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &config_alias_uri);

    httpd_uri_t config_js_uri = { .uri = "/config.js", .method = HTTP_GET, .handler = config_js_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &config_js_uri);

    httpd_uri_t status_uri = { .uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &status_uri);

    httpd_uri_t scan_uri = { .uri = "/scan", .method = HTTP_GET, .handler = scan_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &scan_uri);

    httpd_uri_t mdns_get_uri = { .uri = "/mdns", .method = HTTP_GET, .handler = mdns_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &mdns_get_uri);

    httpd_uri_t mdns_set_uri = { .uri = "/mdns", .method = HTTP_POST, .handler = mdns_set_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &mdns_set_uri);

    httpd_uri_t save_uri = { .uri = "/save", .method = HTTP_POST, .handler = save_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &save_uri);

    httpd_uri_t forget_uri = { .uri = "/forget", .method = HTTP_POST, .handler = forget_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &forget_uri);

    httpd_uri_t restart_uri = { .uri = "/restart", .method = HTTP_POST, .handler = restart_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &restart_uri);
    // ----------------------------------------------------
    // ---------------------------------------

    ESP_LOGI(TAG, "WebSocket server started successfully");
    return server;
}

// --- INIT ---
void init_webserver(void) {
    ESP_LOGI(TAG, "Webserver Init...");
    
    // WIEDER AKTIVIEREN: mDNS starten
    initialise_mdns(); 
    
    server = start_wss_echo_server();
    
    if (server == NULL) {
        ESP_LOGE(TAG, "Fehler beim Starten des Servers!");
        return;
    }
}

void send_json_to_clients(httpd_handle_t server, const char *json_str) {
    if (server == NULL) return;
    size_t clients = max_open_sockets;
    int client_fds[max_open_sockets];
    if (httpd_get_client_list(server, &clients, client_fds) == ESP_OK) {
        for (size_t i = 0; i < clients; ++i) {
            int sock = client_fds[i];
            if (httpd_ws_get_fd_info(server, sock) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_frame_t ws_pkt = {
                    .final = true, .fragmented = false, .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)json_str, .len = strlen(json_str)
                };
                httpd_ws_send_frame_async(server, sock, &ws_pkt);
            }
        }
    }
}

httpd_handle_t get_webserver_handle(void) {
    return server;
}
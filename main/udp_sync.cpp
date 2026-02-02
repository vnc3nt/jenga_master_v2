#include "udp_sync.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "global_vars.h"
#include "esp_netif.h"
#include <vector>
#include <string.h>
#include <algorithm>
#include <sys/errno.h>
#include <arpa/inet.h>
#include "esp_timer.h"

static const char *TAG = "UDP_SYNC";
#define UDP_PORT 4242
#define UDP_BROADCAST_IP "255.255.255.255"

// Wir nutzen eine Standard CRC32 Implementierung (Poly 0xEDB88320)
static uint32_t compute_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320) : (crc >> 1);
        }
    }
    return ~crc;
}

extern std::vector<LeaderboardEntry> leaderboard_countdown;
extern std::vector<LeaderboardEntry> leaderboard_countup;
extern void save_leaderboards_nvs();
extern void broadcast_game_state();

static int sock = -1;

// Helper: Eigene IP abrufen (gegen Self-Echo)
static uint32_t get_local_ip() {
    esp_netif_ip_info_t ip_info;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_get_ip_info(netif, &ip_info);
        return ip_info.ip.addr;
    }
    return 0;
}

void send_udp_packet(const char* json_str) {
    if (sock < 0) return;

    size_t json_len = strlen(json_str);
    size_t total_len = 4 + json_len; // 4 Bytes Header + Payload

    // Puffer erstellen (auf Stack ist okay für UDP < 1500 bytes)
    if (total_len > 1460) {
        ESP_LOGE(TAG, "Packet too large for UDP buffer!");
        return;
    }
    
    uint8_t packet_buf[1460];

    // 1. CRC berechnen
    uint32_t crc = compute_crc32((const uint8_t*)json_str, json_len);
    
    // 2. CRC in Network Byte Order (Big Endian) konvertieren
    uint32_t crc_net = htonl(crc);

    // 3. Header schreiben (Byte 0-3)
    memcpy(packet_buf, &crc_net, 4);

    // 4. Payload schreiben (Byte 4...)
    memcpy(packet_buf + 4, json_str, json_len);

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(UDP_BROADCAST_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);

    int err = sendto(sock, packet_buf, total_len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
    }
}

// --- BROADCAST (Unverändert, ruft jetzt das neue send_udp_packet auf) ---
void broadcast_leaderboard_udp() {
    if (xSemaphoreTake(leaderboard_mutex, portMAX_DELAY)) {
        auto send_vector = [](const std::vector<LeaderboardEntry>& vec, int mode) {
            int count = 0;
            cJSON *root = NULL;
            cJSON *arr = NULL;
            for (const auto& entry : vec) {
                if (count % 5 == 0) {
                    if (root) {
                        char *str = cJSON_PrintUnformatted(root);
                        send_udp_packet(str);
                        free(str);
                        cJSON_Delete(root);
                    }
                    root = cJSON_CreateObject();
                    cJSON_AddStringToObject(root, "type", "sync_lb");
                    cJSON_AddNumberToObject(root, "mode", mode);
                    arr = cJSON_CreateArray();
                    cJSON_AddItemToObject(root, "entries", arr);
                }
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "team", entry.team_name);
                cJSON_AddNumberToObject(item, "time", (double)entry.time_ms);
                cJSON_AddNumberToObject(item, "moves", entry.moves);
                cJSON_AddBoolToObject(item, "fell", entry.tower_fell);
                cJSON_AddNumberToObject(item, "id", entry.entry_id);
                cJSON_AddNumberToObject(item, "ts", (double)entry.timestamp);
                cJSON_AddItemToArray(arr, item);
                count++;
            }
            if (root) {
                char *str = cJSON_PrintUnformatted(root);
                send_udp_packet(str);
                free(str);
                cJSON_Delete(root);
            }
        };
        send_vector(leaderboard_countdown, 0);
        send_vector(leaderboard_countup, 1);
        xSemaphoreGive(leaderboard_mutex);
    }
}

// --- PROCESS INCOMING (Unverändert, aber sicherheitshalber hier nochmal für Copy-Paste) ---
void process_incoming_entries(cJSON *entries_arr, int mode) {
    bool changed = false;
    std::vector<LeaderboardEntry>* target_vec = (mode == 0) ? &leaderboard_countdown : &leaderboard_countup;

    if (xSemaphoreTake(leaderboard_mutex, portMAX_DELAY)) {
        int arr_len = cJSON_GetArraySize(entries_arr);
        for(int i=0; i<arr_len; i++) {
            cJSON *item = cJSON_GetArrayItem(entries_arr, i);
            cJSON *id_json = cJSON_GetObjectItem(item, "id");
            if (!id_json) continue;
            
            uint32_t rcv_id = (uint32_t)id_json->valuedouble; 
            
            bool found = false;
            for(auto &local_entry : *target_vec) {
                if (local_entry.entry_id == rcv_id) {
                    found = true;
                    // SMART MERGE: Nur aktualisieren wenn Remote Timestamp neuer ist
                    cJSON *ts_json = cJSON_GetObjectItem(item, "ts");
                    int64_t remote_ts = ts_json ? (int64_t)ts_json->valuedouble : 0;
                    
                    if (remote_ts > local_entry.timestamp) {
                        ESP_LOGI(TAG, "Sync: Updating Entry %lu (Newer TS)", (unsigned long)rcv_id);
                        cJSON *team = cJSON_GetObjectItem(item, "team");
                        cJSON *moves = cJSON_GetObjectItem(item, "moves");
                        cJSON *timeVal = cJSON_GetObjectItem(item, "time");
                        cJSON *fell = cJSON_GetObjectItem(item, "fell");

                        if (team) strlcpy(local_entry.team_name, team->valuestring, sizeof(local_entry.team_name));
                        if (moves) local_entry.moves = moves->valueint;
                        if (timeVal) local_entry.time_ms = (int64_t)timeVal->valuedouble;
                        if (fell) local_entry.tower_fell = cJSON_IsTrue(fell);
                        local_entry.timestamp = remote_ts;
                        changed = true;
                    }
                    break;
                }
            }
            
            if (!found) {
                LeaderboardEntry new_entry;
                cJSON *team = cJSON_GetObjectItem(item, "team");
                cJSON *moves = cJSON_GetObjectItem(item, "moves");
                cJSON *timeVal = cJSON_GetObjectItem(item, "time");
                cJSON *fell = cJSON_GetObjectItem(item, "fell");
                cJSON *ts = cJSON_GetObjectItem(item, "ts");

                if (team) strlcpy(new_entry.team_name, team->valuestring, sizeof(new_entry.team_name));
                new_entry.moves = moves ? moves->valueint : 0;
                new_entry.time_ms = timeVal ? (int64_t)timeVal->valuedouble : 0;
                new_entry.tower_fell = fell ? cJSON_IsTrue(fell) : false;
                new_entry.entry_id = rcv_id;
                new_entry.timestamp = ts ? (int64_t)ts->valuedouble : esp_timer_get_time() / 1000;
                
                target_vec->push_back(new_entry);
                changed = true;
            }
        }
        
        if (changed) {
             std::sort(target_vec->begin(), target_vec->end(), [](const LeaderboardEntry& a, const LeaderboardEntry& b) {
                if (a.moves != b.moves) return a.moves > b.moves;
                return a.time_ms < b.time_ms;
            });
            if (target_vec->size() > 20) target_vec->resize(20);
            save_leaderboards_nvs();
        }
        xSemaphoreGive(leaderboard_mutex);
        
        if (changed) broadcast_game_state(); 
    }
}

// --- SERVER TASK MIT CRC CHECK ---
static void udp_server_task(void *pvParameters) {
    uint8_t rx_buffer[1500]; // Raw Bytes, nicht char!
    
    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);

        if (len < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        else if (len > 0) {
            // 1. IP Check (Self-Echo)
            uint32_t my_ip = get_local_ip();
            if (source_addr.sin_addr.s_addr == my_ip) continue; 

            // 2. Length Check (Header muss min 4 bytes sein)
            if (len <= 4) {
                ESP_LOGW(TAG, "Packet too short, discarding.");
                continue;
            }

            // 3. CRC Extraktion
            uint32_t received_crc_net;
            memcpy(&received_crc_net, rx_buffer, 4);
            uint32_t received_crc = ntohl(received_crc_net);

            // 4. Payload Pointer und Länge
            uint8_t *payload = rx_buffer + 4;
            size_t payload_len = len - 4;

            // 5. CRC Nachrechnen
            uint32_t calculated_crc = compute_crc32(payload, payload_len);

            if (calculated_crc != received_crc) {
                ESP_LOGE(TAG, "CRC Mismatch! Exp: %08lx, Got: %08lx. Discarding.", (unsigned long)calculated_crc, (unsigned long)received_crc);
                continue; // Paket verwerfen!
            }

            // 6. Null-Termination für cJSON (wir nutzen den Puffer einfach weiter)
            // Sicherstellen, dass wir nicht out-of-bounds schreiben
            if (len < sizeof(rx_buffer)) {
                rx_buffer[len] = 0; 
            } else {
                rx_buffer[sizeof(rx_buffer)-1] = 0;
            }

            // 7. Parsing
            // Payload ist jetzt ein gültiger JSON String
            cJSON *root = cJSON_Parse((char*)payload);
            if (root) {
                cJSON *type = cJSON_GetObjectItem(root, "type");
                if (type && strcmp(type->valuestring, "sync_lb") == 0) {
                    cJSON *mode = cJSON_GetObjectItem(root, "mode");
                    cJSON *entries = cJSON_GetObjectItem(root, "entries");
                    if (mode && entries && cJSON_IsArray(entries)) {
                        process_incoming_entries(entries, mode->valueint);
                    }
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "JSON Parse Error despite valid CRC");
            }
        }
    }
}

void init_udp_sync() {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) return;

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);
    
    if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) return;

    xTaskCreate(udp_server_task, "udp_server", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "UDP Sync (Secure with CRC32) initialized on port %d", UDP_PORT);
}
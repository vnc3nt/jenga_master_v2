#include "webserver.h"
#include "config_wifi.h"
#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <esp_log.h>
#include <cJSON.h>
#include <esp_sleep.h>
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include <vector>
#include <string>
#include <string.h>
#include <algorithm>

#include "global_vars.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "udp_sync.h"

// Forward Declaration
void broadcast_game_state();

static const char *TAG = "MAIN"; // Tag für Logs
SemaphoreHandle_t leaderboard_mutex = NULL; // Globale Definition

// Pin Definitionen
const gpio_num_t PAUSE_PIN = GPIO_NUM_5;
const gpio_num_t CONNECTION_LED_PIN = GPIO_NUM_2;
const gpio_num_t PAUSE_LED_PIN = GPIO_NUM_3;
const gpio_num_t ROBOT_PIN = GPIO_NUM_14;
//const gpio_num_t POWER_LED_PIN = GPIO_NUM_21; in global_vars.h

#define LONG_PRESS_TIME 3000 // 3 Sekunden
bool still_startup_holding = true;
bool is_in_config_mode = false;
volatile bool is_locating_active = false; // Flag für Locate-Animation

// --- NEUE SPIEL VARIABLEN ---
// LeaderboardEntry ist jetzt in global_vars.h definiert!

std::vector<LeaderboardEntry> leaderboard_countdown;
std::vector<LeaderboardEntry> leaderboard_countup;

enum GameMode { MODE_A_COUNTDOWN, MODE_B_COUNTUP };
GameMode current_game_mode = MODE_A_COUNTDOWN;

volatile int piece_counter = 0;
volatile int64_t time_countdown = 0; // in Millisekunden (int64 für einfache Berechnung)
volatile int64_t time_countup = 0;   // in Millisekunden
volatile bool is_paused = true;      // Startet pausiert
char current_team_name[32] = "";     // NEU: Aktueller Teamname
uint32_t countdown_start_value = 5 * 60 * 1000; // Standard 5 Minuten

// Hilfsvariablen für Flankenerkennung
bool last_robot_pin_state = false;

httpd_handle_t server_handle = NULL;

void initialize_gpios() {
    //GPIOs
    gpio_reset_pin(PAUSE_LED_PIN);
    gpio_reset_pin(CONNECTION_LED_PIN);
    gpio_reset_pin(POWER_LED_PIN);
    gpio_reset_pin(PAUSE_PIN);
    gpio_reset_pin(ROBOT_PIN);

    // Als Output setzen
    gpio_set_direction(PAUSE_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(POWER_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(CONNECTION_LED_PIN, GPIO_MODE_OUTPUT);

    // Default LED states
    gpio_set_level(CONNECTION_LED_PIN, 0);
    
    // Als Input setzen (mit Pullup, da wir auf LOW prüfen)
    gpio_set_direction(PAUSE_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PAUSE_PIN, GPIO_PULLUP_ONLY);

    // Robot Pin als Input (High-Aktiv -> Pulldown, um Floating zu vermeiden)
    gpio_set_direction(ROBOT_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(ROBOT_PIN, GPIO_PULLDOWN_ONLY);

    gpio_set_level(POWER_LED_PIN, 1); // Power-LED an
}

void start_config_mode() {
    is_in_config_mode = true;
    ESP_LOGI(TAG, "Konfigurationsmodus aktiviert!");
    start_config_wifi();
}

void stop_config_mode() {
    is_in_config_mode = false;
    ESP_LOGI(TAG, "Konfigurationsmodus deaktiviert!");
    stop_config_wifi();
}

void shutdown_esp() {
    ESP_LOGW(TAG, "Gehe in den Standby (Light Sleep)...");
    
    // 1. Webserver stoppen, um Sauberkeit zu wahren
    if (server_handle != NULL) {
        httpd_stop(server_handle);
        server_handle = NULL;
    }

    // 2. WiFi stoppen spart viel Strom
    esp_wifi_stop();
    
    // 3. LEDs aus
    gpio_set_level(POWER_LED_PIN, 0); 
    gpio_set_level(PAUSE_LED_PIN, 0);
    gpio_set_level(CONNECTION_LED_PIN, 0);

    // 4. Warten, bis der Knopf losgelassen wurde (sonst wacht er sofort wieder auf)
    while (gpio_get_level(PAUSE_PIN) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Kurzes Debounce Delay
    vTaskDelay(pdMS_TO_TICKS(50));

    // 5. Wakeup konfigurieren (Funktioniert mit JEDEM Pin im Light Sleep)
    // GPIO_INTR_LOW_LEVEL: Wacht auf, wenn Pin auf GND gezogen wird
    gpio_wakeup_enable(PAUSE_PIN, GPIO_INTR_LOW_LEVEL);
    
    // Wakeup-Quelle aktivieren
    esp_sleep_enable_gpio_wakeup();

    // 6. Gute Nacht (Light Sleep)
    // Der Code pausiert hier, bis der Knopf gedrückt wird.
    esp_light_sleep_start();

    // ---------------------------------------------------------
    // HIER landet der Code sofort nach dem Aufwachen!
    // ---------------------------------------------------------
    
    ESP_LOGI(TAG, "Aufgewacht! Führe Neustart durch...");
    
    // Da Deep Sleep normalerweise alles resetet, simulieren wir das hier,
    // damit Ihre Logik in app_main() sauber von vorne beginnt.
    esp_restart();
}

// --- NVS FUNKTIONEN (Angepasst) ---

// Zum Speichern der Countdown-Startzeit
void save_countdown_start(uint32_t time_ms) {
    ESP_LOGI(TAG, "Speichere Countdown Startzeit: %lu ms", time_ms);
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return;
    
    err = nvs_set_u32(my_handle, "cd_start", time_ms);
    err = nvs_commit(my_handle);
    nvs_close(my_handle);
}

// Zum Laden der Countdown-Startzeit
uint32_t load_countdown_start(uint32_t default_value) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err != ESP_OK) return default_value;
    
    uint32_t val;
    err = nvs_get_u32(my_handle, "cd_start", &val);
    if (err != ESP_OK) val = default_value;
    
    nvs_close(my_handle);
    ESP_LOGI(TAG, "Lade Countdown Startzeit: %lu ms", val);
    return val;
}

// --- NVS: TEAM NAME & LEADERBOARD ---
void save_team_name_nvs(const char* name) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) != ESP_OK) return;
    nvs_set_str(my_handle, "team_name", name);
    nvs_commit(my_handle);
    nvs_close(my_handle);
}

void load_team_name_nvs(char* buf, size_t len) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) != ESP_OK) return;
    size_t required_size = 0;
    if (nvs_get_str(my_handle, "team_name", NULL, &required_size) == ESP_OK) {
        if (required_size <= len) {
             nvs_get_str(my_handle, "team_name", buf, &required_size);
        }
    }
    nvs_close(my_handle);
}

void save_leaderboards_nvs() {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) != ESP_OK) return;

    // Countdown (max 1KB blob just to be safe, though vector can be large. NVS key max val is 4000ish bytes usually fine for top 10)
    // We only save up to 20 entries to prevent overflow
    if (leaderboard_countdown.size() > 20) leaderboard_countdown.resize(20);
    nvs_set_blob(my_handle, "lb_cd", leaderboard_countdown.data(), leaderboard_countdown.size() * sizeof(LeaderboardEntry));
    
    // Countup
    if (leaderboard_countup.size() > 20) leaderboard_countup.resize(20);
    nvs_set_blob(my_handle, "lb_cu", leaderboard_countup.data(), leaderboard_countup.size() * sizeof(LeaderboardEntry));

    nvs_commit(my_handle);
    nvs_close(my_handle);
    ESP_LOGI(TAG, "Leaderboards saved to NVS.");
}

void load_leaderboards_nvs() {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) != ESP_OK) return;

    size_t required_size = 0;
    // Countdown
    if (nvs_get_blob(my_handle, "lb_cd", NULL, &required_size) == ESP_OK && required_size > 0) {
        size_t count = required_size / sizeof(LeaderboardEntry);
        leaderboard_countdown.resize(count);
        nvs_get_blob(my_handle, "lb_cd", leaderboard_countdown.data(), &required_size);
        ESP_LOGI(TAG, "Loaded %d entries for Countdown Leaderboard", count);
    }
    // Countup
    required_size = 0;
    if (nvs_get_blob(my_handle, "lb_cu", NULL, &required_size) == ESP_OK && required_size > 0) {
        size_t count = required_size / sizeof(LeaderboardEntry);
        leaderboard_countup.resize(count);
        nvs_get_blob(my_handle, "lb_cu", leaderboard_countup.data(), &required_size);
         ESP_LOGI(TAG, "Loaded %d entries for Countup Leaderboard", count);
    }
    nvs_close(my_handle);
}

// --- API FÜR WEBSERVER (Vorbereitung) ---

void api_set_paused(bool paused) {
    if (is_paused != paused) {
        is_paused = paused;
        ESP_LOGI(TAG, "API: Pause Status geändert auf: %s", is_paused ? "PAUSIERT" : "LAUFEND");
    }
}

void api_set_countdown_manual(uint32_t new_time_ms) {
    // Überschreibt Dauerspeicher und aktuelle Zeit
    save_countdown_start(new_time_ms);
    countdown_start_value = new_time_ms;
    time_countdown = new_time_ms;
    ESP_LOGI(TAG, "API: Countdown manuell gesetzt auf: %lu ms", new_time_ms);
}

void api_increment_piece_counter() {
    piece_counter = piece_counter + 1;
    ESP_LOGI(TAG, "API: Stückzähler inkrementiert auf: %d", piece_counter);
}

void api_decrement_piece_counter() {
    // HIER: Sicherheitsabfrage hinzufügen
    if (piece_counter > 0) {
        piece_counter = piece_counter - 1;
        ESP_LOGI(TAG, "API: Stückzähler dekrementiert auf: %d", piece_counter);
    } else {
        ESP_LOGW(TAG, "API: Dekrementierung ignoriert, Counter ist bereits 0");
    }
}

// API um den Spielmodus zu setzen
void api_set_game_mode(int mode) {
    if (mode == 0) {
        current_game_mode = MODE_A_COUNTDOWN;
        ESP_LOGI(TAG, "API: Modus auf COUNTDOWN (A) gesetzt");
    } else {
        current_game_mode = MODE_B_COUNTUP;
        ESP_LOGI(TAG, "API: Modus auf COUNTUP (B) gesetzt");
    }
}

// NEU: API um Zeit zurückzusetzen
void api_reset_time() {
    // 1. Züge immer zurücksetzen
    piece_counter = 0;
    ESP_LOGI(TAG, "API: Züge auf 0 zurückgesetzt");
    // Wichtig: Teamname hier NICHT löschen, da er für das neue Spiel erhalten bleiben soll.
    // Aber vielleicht beim allerersten Start erzwingen? 
    // Nein, wir gehen davon aus, dass er gesetzt bleibt.

    // 2. Zeit je nach Modus zurücksetzen
    if (current_game_mode == MODE_A_COUNTDOWN) {
        time_countdown = countdown_start_value;
        ESP_LOGI(TAG, "API: Zeit Reset (Countdown) auf %lu ms", countdown_start_value);
    } else {
        time_countup = 0;
        ESP_LOGI(TAG, "API: Zeit Reset (Countup) auf 0 ms");
    }
}

void api_set_team_name(const char* team_name) {
    strlcpy(current_team_name, team_name, sizeof(current_team_name));
    save_team_name_nvs(current_team_name); // Persistenz
    ESP_LOGI(TAG, "API: Teamname gesetzt auf: '%s'", current_team_name);
}

void api_save_and_reset(const char* team_name, bool tower_fell) {
    LeaderboardEntry entry;
    // Sicherstellen, dass der String terminiert ist und nicht überläuft
    strlcpy(entry.team_name, team_name, sizeof(entry.team_name));
    entry.moves = piece_counter;
    entry.tower_fell = tower_fell;
    
    // NEU: Unique ID und Timestamp
    entry.entry_id = esp_random();
    entry.timestamp = esp_timer_get_time() / 1000; // ms seit Boot

    if (current_game_mode == MODE_A_COUNTDOWN) {
        // Berechne vergangene Zeit: Startzeit - verbleibende Zeit
        entry.time_ms = countdown_start_value - time_countdown;
        leaderboard_countdown.push_back(entry);
        // Sortieren: Züge absteigend (mehr = besser), bei Gleichstand Zeit aufsteigend (weniger = besser)
        std::sort(leaderboard_countdown.begin(), leaderboard_countdown.end(), [](const LeaderboardEntry& a, const LeaderboardEntry& b) {
            if (a.moves != b.moves) {
                return a.moves > b.moves;
            }
            return a.time_ms < b.time_ms;
        });
        ESP_LOGI(TAG, "Saved entry to Countdown Leaderboard. Total entries: %d", leaderboard_countdown.size());
    } else {
        entry.time_ms = time_countup;
        leaderboard_countup.push_back(entry);
        // Sortieren: Züge absteigend (mehr = besser), bei Gleichstand Zeit aufsteigend (weniger = besser)
        std::sort(leaderboard_countup.begin(), leaderboard_countup.end(), [](const LeaderboardEntry& a, const LeaderboardEntry& b) {
            if (a.moves != b.moves) {
                return a.moves > b.moves;
            }
            return a.time_ms < b.time_ms;
        });
        ESP_LOGI(TAG, "Saved entry to Countup Leaderboard. Total entries: %d", leaderboard_countup.size());
    }

    save_leaderboards_nvs(); // Leaderboard sofort speichern
    // UDP Broadcast senden (für Sync mit anderen Geräten)
    broadcast_leaderboard_udp();
    
    api_reset_time();
}

// --- NEU: LEADERBOARD EDIT API ---

void api_update_entry(uint32_t id, const char* team, int moves, int64_t time_ms, bool fell) {
    ESP_LOGI(TAG, "API Update Entry: ID=%lu, Team=%s", (unsigned long)id, team);
    xSemaphoreTake(leaderboard_mutex, portMAX_DELAY);
    
    bool found = false;
    // Check Countdown vector
    for (auto& e : leaderboard_countdown) {
        if (e.entry_id == id) {
            strlcpy(e.team_name, team, sizeof(e.team_name));
            e.moves = moves;
            e.time_ms = time_ms;
            e.tower_fell = fell;
            found = true;
            break;
        }
    }
    
    // Check Countup vector if not found
    if (!found) {
        for (auto& e : leaderboard_countup) {
            if (e.entry_id == id) {
                strlcpy(e.team_name, team, sizeof(e.team_name));
                e.moves = moves;
                e.time_ms = time_ms;
                e.tower_fell = fell;
                found = true;
                break;
            }
        }
    }
    
    if (found) {
        ESP_LOGI(TAG, "Entry %lu updated.", (unsigned long)id);

        // Re-Sortieren um Konsistenz zu wahren
        std::sort(leaderboard_countdown.begin(), leaderboard_countdown.end(), [](const LeaderboardEntry& a, const LeaderboardEntry& b) {
            if (a.moves != b.moves) return a.moves > b.moves;
            return a.time_ms < b.time_ms;
        });
        std::sort(leaderboard_countup.begin(), leaderboard_countup.end(), [](const LeaderboardEntry& a, const LeaderboardEntry& b) {
            if (a.moves != b.moves) return a.moves > b.moves;
            return a.time_ms < b.time_ms;
        });

        save_leaderboards_nvs();
        // broadcast_leaderboard_udp(); <-- ENTFERNT (Deadlock Gefahr!)
    } else {
        ESP_LOGW(TAG, "Entry %lu not found for update.", (unsigned long)id);
    }
    
    xSemaphoreGive(leaderboard_mutex);
    
    if (found) broadcast_leaderboard_udp(); // Jetzt sicher aufrufen
    broadcast_game_state();
}

void api_delete_entry(uint32_t id) {
     xSemaphoreTake(leaderboard_mutex, portMAX_DELAY);
     
     // Remove from Countdown
     auto it = std::remove_if(leaderboard_countdown.begin(), leaderboard_countdown.end(), 
                              [id](const LeaderboardEntry& e){ return e.entry_id == id; });
     if (it != leaderboard_countdown.end()) {
         leaderboard_countdown.erase(it, leaderboard_countdown.end());
         ESP_LOGI(TAG, "Entry %lu deleted from Countdown.", (unsigned long)id);
     }
     
     // Remove from Countup
     auto it2 = std::remove_if(leaderboard_countup.begin(), leaderboard_countup.end(), 
                              [id](const LeaderboardEntry& e){ return e.entry_id == id; });
     if (it2 != leaderboard_countup.end()) {
         leaderboard_countup.erase(it2, leaderboard_countup.end());
         ESP_LOGI(TAG, "Entry %lu deleted from Countup.", (unsigned long)id);
     }
     
     save_leaderboards_nvs();
     // broadcast_leaderboard_udp(); // deadlock fix
     
     xSemaphoreGive(leaderboard_mutex);

     broadcast_leaderboard_udp();
     broadcast_game_state();
}

void api_clear_leaderboard() {
    xSemaphoreTake(leaderboard_mutex, portMAX_DELAY);
    
    if (current_game_mode == MODE_A_COUNTDOWN) {
        leaderboard_countdown.clear();
        ESP_LOGI(TAG, "Countdown leaderboard cleared.");
    } else {
        leaderboard_countup.clear();
        ESP_LOGI(TAG, "Countup leaderboard cleared.");
    }
    
    save_leaderboards_nvs();
    // broadcast_leaderboard_udp(); // deadlock fix
    
    xSemaphoreGive(leaderboard_mutex);

    broadcast_leaderboard_udp();
    broadcast_game_state();
}

// Getter Funktionen (können vom Webserver genutzt werden)
int api_get_piece_counter() { return piece_counter; }
int64_t api_get_time_countdown() { return time_countdown; }
int64_t api_get_time_countup() { return time_countup; }
bool api_get_is_paused() { return is_paused; }
int api_get_game_mode() { return (int)current_game_mode; }

// --- LOCATE FUNCTION ---
void api_trigger_locate() {
    ESP_LOGI(TAG, "Locate trigger received! Blinking LED...");
    is_locating_active = true;
    
    // 3 x Blinken (Blockierend ist hier okay, da nur kurz)
    for(int i=0; i<3; i++) {
        gpio_set_level(CONNECTION_LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(CONNECTION_LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    is_locating_active = false;
}

// --- LOGIK FUNKTIONEN ---

void toggle_pause() {
    // NEU: Wenn kein Teamname gesetzt ist, darf Spiel nicht gestartet werden
    if (is_paused == true) { // Wir sind momentan pausiert und wollen starten
        if (strlen(current_team_name) == 0) {
            ESP_LOGW(TAG, "Start verhindert: Kein Teamname gesetzt!");
            // Kurzes Blinken als Fehlersignal
            for(int i=0; i<3; i++) {
                gpio_set_level(PAUSE_LED_PIN, 0); vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(PAUSE_LED_PIN, 1); vTaskDelay(pdMS_TO_TICKS(100));
            }
            return;
        }
    }
    
    is_paused = !is_paused;
    ESP_LOGI(TAG, "Spielstatus geändert: %s", is_paused ? "PAUSIERT" : "LAUFEND");
    
    // LED Logik sofort aktualisieren
    if (is_paused) {
        gpio_set_level(PAUSE_LED_PIN, 1); // An wenn pausiert
    } else {
        gpio_set_level(PAUSE_LED_PIN, 0); // Aus wenn läuft
    }
}

// Erweiterte Button Logik (Kurz = Pause, Lang = Shutdown)
void handle_button_logic() {
    if (still_startup_holding) return;
    
    static uint64_t press_start = 0;
    static bool last_state = 1; // Pullup -> 1 ist losgelassen
    bool current_state = gpio_get_level(PAUSE_PIN);

    // Flankenerkennung: Drücken (1 -> 0)
    if (last_state == 1 && current_state == 0) {
        press_start = esp_timer_get_time() / 1000; // ms
    }
    // Halten (0 -> 0)
    else if (last_state == 0 && current_state == 0) {
        if ((esp_timer_get_time() / 1000) - press_start > LONG_PRESS_TIME) {
            ESP_LOGI(TAG, "Long press detected -> Shutdown");
            shutdown_esp();
        }
    }
    // Loslassen (0 -> 1)
    else if (last_state == 0 && current_state == 1) {
        uint64_t press_duration = (esp_timer_get_time() / 1000) - press_start;
        if (press_duration < LONG_PRESS_TIME && press_duration > 50) { // >50ms debounce
            toggle_pause();
        }
        press_start = 0;
    }
    last_state = current_state;
}

// --- NEUE FUNKTION: Status senden ---
void broadcast_game_state() {
    cJSON *root = cJSON_CreateObject();
    
    // NEU: Team Name Info (damit Client weiß, ob ESP bereit ist)
    cJSON_AddStringToObject(root, "current_team", current_team_name);

    // NEU: Server Timestamp für Heartbeat / Connection Check
    cJSON_AddNumberToObject(root, "server_timestamp", (double)(esp_timer_get_time() / 1000));
    
    // 1. Modus
    cJSON_AddNumberToObject(root, "mode", (int)current_game_mode);
    
    // 2. Zeiten
    cJSON_AddNumberToObject(root, "time_countdown", (double)time_countdown);
    cJSON_AddNumberToObject(root, "time_countup", (double)time_countup);
    cJSON_AddNumberToObject(root, "countdown_start", (double)countdown_start_value);
    
    // 3. Status
    cJSON_AddBoolToObject(root, "is_paused", is_paused);
    
    // 4. Counter
    cJSON_AddNumberToObject(root, "piece_counter", piece_counter);

    // 5. Leaderboard (Aktueller Modus)
    cJSON *lbFuncArr = cJSON_CreateArray();
    const auto& currentLboard = (current_game_mode == MODE_A_COUNTDOWN) ? leaderboard_countdown : leaderboard_countup;
    
    // Mutex für Read Access
    if (xSemaphoreTake(leaderboard_mutex, portMAX_DELAY)) {
        // Begrenzen auf Top 10 um Paketgröße klein zu halten
        int count = 0;
        for (const auto& entry : currentLboard) {
            if (count >= 10) break;
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "team", entry.team_name);
            cJSON_AddNumberToObject(item, "time", (double)entry.time_ms);
            cJSON_AddNumberToObject(item, "moves", entry.moves);
            cJSON_AddBoolToObject(item, "fell", entry.tower_fell);
            cJSON_AddNumberToObject(item, "id", entry.entry_id); // Unique ID
            cJSON_AddNumberToObject(item, "ts", (double)entry.timestamp); // Timestamp
            cJSON_AddItemToArray(lbFuncArr, item);
            count++;
        }
        xSemaphoreGive(leaderboard_mutex);
    }
    cJSON_AddItemToObject(root, "leaderboard", lbFuncArr);

    // JSON String erstellen
    char *json_str = cJSON_PrintUnformatted(root);
    
    // Senden (Funktion muss in webserver.cpp implementiert sein!)
    if (json_str != NULL) {
        ws_broadcast(json_str);
        free(json_str); // WICHTIG: Speicher freigeben
    }
    
    cJSON_Delete(root);
}

// --- MAIN ---

// Initialisierung des NVS-Speichers
esp_err_t init_nvs() {
    ESP_LOGI(TAG, "Initialisiere NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    return ret;
}

extern "C" void app_main(void) {
    // Optional: Warten auf Serial Monitor
    // vTaskDelay(pdMS_TO_TICKS(2000)); 

    ESP_LOGI(TAG, "--- START ESP ---");

    // initialisation
    init_nvs();
    
    // Lade gespeicherte Zeit und Daten
    countdown_start_value = load_countdown_start(5 * 60 * 1000); // Default 5 min
    load_team_name_nvs(current_team_name, sizeof(current_team_name)); // Teamname laden
    load_leaderboards_nvs(); // Leaderboards laden

    time_countdown = countdown_start_value;
    time_countup = 0;
    
    // 2. WICHTIG: Netzwerk-Stack NUR HIER EINMAL initialisieren
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // Interfaces erstellen wir hier, damit sie global verfügbar sind
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // 3. GPIOs
    gpio_install_isr_service(0);
    initialize_gpios();
    
    // Mutex initialisieren
    leaderboard_mutex = xSemaphoreCreateMutex();
    
    // Initialer LED Status (Pausiert -> LED AN)
    gpio_set_level(PAUSE_LED_PIN, 1);

    // 1. STARTUP LOGIK: Prüfen ob Knopf gehalten wird
    if (gpio_get_level(PAUSE_PIN) == 0) {
        ESP_LOGI(TAG, "Knopf gedrückt. Prüfe auf 3s Halten...");
        int hold_counter = 0;
        while (gpio_get_level(PAUSE_PIN) == 0 && hold_counter < 3000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            hold_counter += 10;
        }

        if (hold_counter >= 3000) {
            ESP_LOGI(TAG, "Knopf > 3s gehalten -> Config Modus.");
            start_config_mode();
            while(gpio_get_level(PAUSE_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            ESP_LOGI(TAG, "Knopf losgelassen. Normaler Start.");
        }
    }
    still_startup_holding = false;

    // 5. VERBINDUNGSAUFBAU
    if (!is_in_config_mode) {
        ESP_LOGI(TAG, "Versuche Verbindung mit gespeichertem WiFi...");
        if (connect_saved_wifi()) {
            ESP_LOGI(TAG, "Erfolgreich verbunden! Starte Webserver...");
            init_webserver(); // Webserver starten!
            init_udp_sync(); // UDP Listener starten
            // Initialer Broadcast
            vTaskDelay(pdMS_TO_TICKS(1000)); // Kurz warten bis Netz stabil
            broadcast_leaderboard_udp();
        } else {
            ESP_LOGW(TAG, "Verbindung fehlgeschlagen. Starte Config Modus.");
            start_config_mode();
        }
    }

    // 6. MAIN LOOP
    bool led_state = false;
    int64_t last_loop_time = esp_timer_get_time();
    int pulse_counter = 0;
    
    // Timer für WebSocket Broadcast (z.B. alle 200ms)
    int64_t ws_timer = 0; 

    ESP_LOGI(TAG, "Starte Game Loop. Modus A (Countdown). Zeit: %lld ms", time_countdown);

    while(1) {
        if (is_in_config_mode) {
            // Im Konfigurationsmodus: Schnelles Blinken
            led_state = !led_state;
            gpio_set_level(CONNECTION_LED_PIN, led_state);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else {
            // --- IM SPIELMODUS ---
            if (!is_locating_active) {
                gpio_set_level(CONNECTION_LED_PIN, 0);
            }
            
            // 1. Zeitberechnung (Delta Time)
            int64_t current_time = esp_timer_get_time();
            int64_t delta_us = current_time - last_loop_time;
            last_loop_time = current_time;
            int64_t delta_ms = delta_us / 1000;

            // 2. SPIEL LOGIK (Zeit & Roboter)
            if (!is_paused) {
                // A) Countdown
                if (current_game_mode == MODE_A_COUNTDOWN) {
                    time_countdown -= delta_ms;
                    
                    // Wenn Zeit abgelaufen ist
                    if (time_countdown <= 0) {
                        time_countdown = 0;
                        is_paused = true; // Automatisch pausieren (Game Over)
                        ESP_LOGI(TAG, "Countdown abgelaufen! Spiel pausiert.");
                    }
                } 
                // B) Countup
                else {
                    time_countup += delta_ms;
                }

                // C) Roboter Signal Erkennung (Nur wenn Spiel läuft)
                bool robot_signal = gpio_get_level(ROBOT_PIN);
                if (robot_signal && !last_robot_pin_state) {
                    piece_counter = piece_counter + 1;
                    ESP_LOGI(TAG, "ROBOT SIGNAL! Piece Counter: %d", piece_counter);
                }
                last_robot_pin_state = robot_signal;
            }

            // 3. LED LOGIK (Hier wird entschieden, was die LED macht)
            
            // Priorität 1: Zeit abgelaufen (Alarm) -> BLINKEN
            if (current_game_mode == MODE_A_COUNTDOWN && time_countdown <= 0) {
                // Schnelles Blinken (100ms Takt)
                bool blink_state = (current_time / 100000) % 2;
                gpio_set_level(PAUSE_LED_PIN, blink_state);
            }
            // Priorität 2: Normal Pausiert -> DAUERHAFT AN
            else if (is_paused) {
                gpio_set_level(PAUSE_LED_PIN, 1);
            }
            // Priorität 3: Spiel läuft -> AUS
            else {
                gpio_set_level(PAUSE_LED_PIN, 0);
            }

            // 4. WebSocket Broadcast Timer
            ws_timer += delta_ms;
            if (ws_timer >= 250) { 
                broadcast_game_state();
                ws_timer = 0;
            }

            // 5. Button Logik (Pause / Shutdown)
            handle_button_logic();
            
            // Debug Ausgabe (alle 5 Sekunden)
            static int64_t log_timer = 0;
            log_timer += delta_ms;
            if (log_timer > 5000) {
                ESP_LOGI(TAG, "Status: %s | Mode: %s | CD: %lld ms | CU: %lld ms | Pieces: %d", 
                         is_paused ? "PAUSE" : "RUN", 
                         (current_game_mode == MODE_A_COUNTDOWN) ? "A (Down)" : "B (Up)",
                         time_countdown, time_countup, piece_counter);
                log_timer = 0;
            }

            // WICHTIG: Kurze Pause für Watchdog
            vTaskDelay(pdMS_TO_TICKS(10));
        }   
    }
}

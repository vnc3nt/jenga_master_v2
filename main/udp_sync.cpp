#include "udp_sync.h"
#include <esp_log.h>

static const char *TAG = "UDP_DUMMY";

// Da wir auf Master/Client Architektur mit HTTP umgestiegen sind, 
// wird UDP Broadcast hier deaktiviert, um Build-Fehler zu vermeiden.

void init_udp_sync() {
    ESP_LOGI(TAG, "UDP Sync deaktiviert (nicht benötigt in V2).");
}

void broadcast_leaderboard_udp() {
    // Leer lassen
}

void broadcast_game_state_udp() {
    // Leer lassen
}
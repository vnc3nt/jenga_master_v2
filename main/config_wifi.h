#ifndef CONFIG_WIFI_H
#define CONFIG_WIFI_H

#include <esp_err.h>

// Die neue Haupt-Funktion für das Netzwerk (Wählt automatisch AP oder STA basierend auf is_master)
void setup_network();

// Alte Funktionen als Dummies behalten, falls noch Referenzen existieren
void start_config_wifi();
void stop_config_wifi();
bool connect_saved_wifi();

#endif
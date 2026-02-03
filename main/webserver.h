#ifndef WEBSERVER_H
#define WEBSERVER_H
#include <esp_http_server.h>

// Getter-Funktion für den WebSocket-Server-Handle
httpd_handle_t get_webserver_handle(void);

// Funktion zum Senden einer JSON-Nachricht an alle verbundenen WebSocket-Clients
void send_json_to_clients(httpd_handle_t server, const char *json_str);

// Funktion zum Initialisieren des Web-Servers
void init_webserver(void);

// Diese Funktion muss in webserver.cpp implementiert sein!
extern void ws_broadcast(const char* str);

void load_leaderboard_nvs();
void save_leaderboard_nvs();

#endif




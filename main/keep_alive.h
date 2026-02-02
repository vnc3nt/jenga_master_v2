#pragma once

#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wss_keep_alive_storage *wss_keep_alive_t;

typedef bool (*wss_check_client_alive_cb_t)(wss_keep_alive_t h, int fd);
typedef bool (*wss_check_client_not_alive_cb_t)(wss_keep_alive_t h, int fd);

typedef struct {
    wss_check_client_alive_cb_t check_client_alive_cb;
    wss_check_client_not_alive_cb_t client_not_alive_cb;
    size_t max_clients;
    size_t keep_alive_period_ms;
    size_t not_alive_after_ms;
} wss_keep_alive_config_t;

#define KEEP_ALIVE_CONFIG_DEFAULT() \
    { \
        .check_client_alive_cb = NULL, \
        .client_not_alive_cb = NULL, \
        .max_clients = 10, \
        .keep_alive_period_ms = 5000, \
        .not_alive_after_ms = 10000, \
    }

/**
 * @brief Startet den Keep-Alive Service
 */
wss_keep_alive_t wss_keep_alive_start(wss_keep_alive_config_t *config);

/**
 * @brief Stoppt den Keep-Alive Service
 */
void wss_keep_alive_stop(wss_keep_alive_t h);

/**
 * @brief Fügt einen Client hinzu (wenn Verbindung geöffnet wird)
 */
esp_err_t wss_keep_alive_add_client(wss_keep_alive_t h, int fd);

/**
 * @brief Entfernt einen Client (wenn Verbindung geschlossen wird)
 */
esp_err_t wss_keep_alive_remove_client(wss_keep_alive_t h, int fd);

/**
 * @brief Meldet Aktivität eines Clients (z.B. PONG empfangen)
 */
esp_err_t wss_keep_alive_client_is_active(wss_keep_alive_t h, int fd);

/**
 * @brief Setzt den User Context (hier: den httpd_handle_t Server)
 */
void wss_keep_alive_set_user_ctx(wss_keep_alive_t h, void *ctx);

/**
 * @brief Holt den User Context zurück
 */
void *wss_keep_alive_get_user_ctx(wss_keep_alive_t h);

#ifdef __cplusplus
}
#endif

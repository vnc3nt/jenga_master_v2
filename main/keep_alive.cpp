#include "keep_alive.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <sys/param.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *TAG = "wss_keep_alive";

struct client_data {
    int fd;
    struct in_addr addr; // IP Adresse zur Identifizierung
    int64_t last_seen;
};

struct wss_keep_alive_storage {
    wss_keep_alive_config_t config;
    struct client_data *clients;
    SemaphoreHandle_t lock;
    esp_timer_handle_t timer;
    void *user_ctx; // <--- WICHTIG: Dieses Feld hat gefehlt!
};

static void keep_alive_timer_cb(void *arg)
{
    wss_keep_alive_t h = (wss_keep_alive_t)arg;
    if (!h) return;

    xSemaphoreTake(h->lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time() / 1000;

    for (size_t i = 0; i < h->config.max_clients; ++i) {
        if (h->clients[i].fd != -1) {
            // Prüfen ob Client zu lange inaktiv war
            if (now - h->clients[i].last_seen > h->config.not_alive_after_ms) {
                if (h->config.client_not_alive_cb) {
                    // Callback aufrufen (schließt Verbindung)
                    bool remove = h->config.client_not_alive_cb(h, h->clients[i].fd);
                    if (remove) {
                        h->clients[i].fd = -1;
                        h->clients[i].last_seen = 0;
                    }
                }
            } else if (now - h->clients[i].last_seen > h->config.keep_alive_period_ms) {
                // Ping senden
                if (h->config.check_client_alive_cb) {
                    h->config.check_client_alive_cb(h, h->clients[i].fd);
                }
            }
        }
    }
    xSemaphoreGive(h->lock);
}

wss_keep_alive_t wss_keep_alive_start(wss_keep_alive_config_t *config)
{
    if (!config) return NULL;

    struct wss_keep_alive_storage *h = (struct wss_keep_alive_storage *)calloc(1, sizeof(struct wss_keep_alive_storage));
    if (!h) return NULL;

    h->config = *config;
    h->clients = (struct client_data *)calloc(config->max_clients, sizeof(struct client_data));
    if (!h->clients) {
        free(h);
        return NULL;
    }
    for (size_t i = 0; i < config->max_clients; ++i) {
        h->clients[i].fd = -1;
    }

    h->lock = xSemaphoreCreateMutex();
    if (!h->lock) {
        free(h->clients);
        free(h);
        return NULL;
    }

    esp_timer_create_args_t timer_args = {
        .callback = keep_alive_timer_cb,
        .arg = h,
        .name = "wss_keep_alive"
    };
    esp_timer_create(&timer_args, &h->timer);
    // Timer starten (Periode = Hälfte der Keep-Alive Zeit, damit wir rechtzeitig pingen)
    esp_timer_start_periodic(h->timer, (config->keep_alive_period_ms / 2) * 1000);

    return h;
}

void wss_keep_alive_stop(wss_keep_alive_t h)
{
    if (!h) return;
    esp_timer_stop(h->timer);
    esp_timer_delete(h->timer);
    vSemaphoreDelete(h->lock);
    free(h->clients);
    free(h);
}

esp_err_t wss_keep_alive_add_client(wss_keep_alive_t h, int fd)
{
    if (!h) return ESP_ERR_INVALID_ARG;

    // 1. IP des neuen Clients ermitteln
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        ESP_LOGE(TAG, "Could not get peer name for fd %d", fd);
        return ESP_FAIL;
    }

    xSemaphoreTake(h->lock, portMAX_DELAY);

    // 2. Prüfen ob IP bereits existiert -> Session übernehmen
    for (size_t i = 0; i < h->config.max_clients; ++i) {
        if (h->clients[i].fd != -1 && h->clients[i].addr.s_addr == addr.sin_addr.s_addr) {
            // Gefunden! Alte Session für diese IP aktualisieren
            ESP_LOGI(TAG, "Client Reconnect detected (IP match). Replacing FD %d with %d", h->clients[i].fd, fd);
            h->clients[i].fd = fd;
            h->clients[i].last_seen = esp_timer_get_time() / 1000;
            xSemaphoreGive(h->lock);
            return ESP_OK;
        }
    }

    // 3. Wenn nicht vorhanden, neuen Slot suchen
    for (size_t i = 0; i < h->config.max_clients; ++i) {
        if (h->clients[i].fd == -1) {
            h->clients[i].fd = fd;
            h->clients[i].addr = addr.sin_addr; // IP speichern
            h->clients[i].last_seen = esp_timer_get_time() / 1000;
            xSemaphoreGive(h->lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(h->lock);
    
    ESP_LOGE(TAG, "Max clients reached, cannot add client %d", fd);
    return ESP_ERR_NO_MEM;
}

esp_err_t wss_keep_alive_remove_client(wss_keep_alive_t h, int fd)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    for (size_t i = 0; i < h->config.max_clients; ++i) {
        if (h->clients[i].fd == fd) {
            h->clients[i].fd = -1;
            h->clients[i].last_seen = 0;
            xSemaphoreGive(h->lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(h->lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t wss_keep_alive_client_is_active(wss_keep_alive_t h, int fd)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    for (size_t i = 0; i < h->config.max_clients; ++i) {
        if (h->clients[i].fd == fd) {
            h->clients[i].last_seen = esp_timer_get_time() / 1000;
            xSemaphoreGive(h->lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(h->lock);
    return ESP_ERR_NOT_FOUND;
}

// --- HIER SIND DIE FEHLENDEN FUNKTIONEN ---

void wss_keep_alive_set_user_ctx(wss_keep_alive_t h, void *ctx)
{
    if (h) {
        h->user_ctx = ctx;
    }
}

void *wss_keep_alive_get_user_ctx(wss_keep_alive_t h)
{
    if (h) {
        return h->user_ctx;
    }
    return NULL;
}


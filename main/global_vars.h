#ifndef GLOBAL_VARS_H
    #define GLOBAL_VARS_H
    
    #include "freertos/FreeRTOS.h"
    #include "freertos/semphr.h"
    #include "driver/gpio.h"
    #include <vector>
    #include <string>
    #include <cstring>
    #include <stdint.h>

    // --- KONFIGURATION ---
    static const bool is_master = true; 

    // PINS
    const gpio_num_t POWER_LED_PIN = GPIO_NUM_21;
    const gpio_num_t PAUSE_LED_PIN = GPIO_NUM_3;
    const gpio_num_t CONNECTION_LED_PIN = GPIO_NUM_2;
    
    const gpio_num_t ROBOT_PIN_1 = GPIO_NUM_14;
    const gpio_num_t ROBOT_PIN_2 = GPIO_NUM_13;
    const gpio_num_t PAUSE_PIN = GPIO_NUM_5;

    extern SemaphoreHandle_t game_mutex;

    struct Robot {
        uint32_t id;
        char name[32];
        int pieces;
        int64_t time_left_ms;
        bool is_running;
        uint32_t client_ip;
        int pin_index;
        int64_t last_seen; 
        bool do_blink; 
    };

    extern std::vector<Robot> robots;
    
    // Status Variablen
    extern bool global_paused;
    extern int64_t global_game_time_ms; // <-- NEU: Unabhängige globale Zeit
    extern int64_t last_loop_time; 

    struct LeaderboardEntry {
        char team_name[32];
        int64_t time_ms;
        int moves;
        uint32_t entry_id; 
        int64_t timestamp; 
    };
    extern std::vector<LeaderboardEntry> leaderboard;

#endif
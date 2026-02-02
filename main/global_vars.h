#ifndef GLOBAL_VARS_H
    #define GLOBAL_VARS_H
    #include "freertos/FreeRTOS.h"
    #include "freertos/semphr.h"
    #include "driver/gpio.h"
    #include <vector>
    #include <string>

    // Configuration
    extern bool is_master; 

    // Pins
    const gpio_num_t POWER_LED_PIN = GPIO_NUM_21;
    const gpio_num_t ROBOT_1_PIN = GPIO_NUM_13;
    const gpio_num_t ROBOT_2_PIN = GPIO_NUM_14;

    // Globals
    extern bool is_game_running; // Global start/stop
    
    // Globaler Mutex für Leaderboard Zugriff
    extern SemaphoreHandle_t leaderboard_mutex;

    // Data Structures
    struct RobotData {
        int id;             // Unique ID (1, 2, 3...)
        char team_name[32];
        int piece_counter;
        int64_t time_countdown_ms;
        bool is_paused; // Individual pause
        int max_moves; // For progress bar tracking (e.g. 36)
        
        // Constructor for defaults
        RobotData() : id(0), piece_counter(0), time_countdown_ms(300000), is_paused(true), max_moves(36) {
             snprintf(team_name, sizeof(team_name), "Robot");
        }
    };

    // Global list of robots (Master keeps track of all, Client only its own locally for buffer)
    extern std::vector<RobotData> all_robots;
    
    // Leaderboard Entry Struktur
    #include <stdint.h>
    struct LeaderboardEntry {
        char team_name[32];
        int64_t avg_time_ms; // Arithmetische Mittel
        int total_moves;
        int attempts_count; 
        uint32_t entry_id;
    };

#endif
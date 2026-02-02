#ifndef UDP_SYNC_H
#define UDP_SYNC_H

#include <stdint.h>

// Initialisiert den UDP Listener Task und den Socket
void init_udp_sync();

// Sendet alle Leaderboard Einträge per UDP Broadcast
void broadcast_leaderboard_udp();

#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <assert.h>
#include <stdbool.h>

#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"

#define BASE_PORT   2201
#define NUM_PORTS   6
#define BUFFER_SIZE 1024

// Track who has acted each street
int has_acted[MAX_PLAYERS] = {0};
int last_raiser = -1;
// Global game state
static game_state_t game;

// Rotate to next active player
static player_id_t next_active_player(void) {
    int cur = game.current_player;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        cur = (cur + 1) % MAX_PLAYERS;
        if (game.player_status[cur] == PLAYER_ACTIVE)
            return cur;
    }
    return -1;
}

// Broadcast INFO packet to all connected clients
static void broadcast_info(void) {
    server_packet_t pkt;
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int fd = game.sockets[pid];
        if (fd < 0) continue;
        build_info_packet(&game, pid, &pkt);
        send(fd, &pkt, sizeof(pkt), 0);
    }
}

// Broadcast END packet to all
static void broadcast_end(int winner) {
    server_packet_t pkt;
    build_end_packet(&game, winner, &pkt);
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int fd = game.sockets[pid];
        if (fd < 0) continue;
        send(fd, &pkt, sizeof(pkt), 0);
    }
}

// Count still-active players
static int count_active_players(void) {
    int cnt = 0;
    for (int p = 0; p < MAX_PLAYERS; ++p)
        if (game.player_status[p] == PLAYER_ACTIVE)
            ++cnt;
    return cnt;
}

// Wait for players to READY or LEAVE; if fewer than 2 remain, send HALT
static bool wait_for_ready(int server_fds[]) {
    int ready_count = 0;
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int fd = game.sockets[pid];
        if (fd < 0) continue;

        client_packet_t in;
        int n = recv(fd, &in, sizeof(in), 0);
        if (n <= 0 || in.packet_type == LEAVE) {
            server_packet_t ack = { .packet_type = ACK };
            send(fd, &ack, sizeof(ack), 0);
            close(fd);
            game.sockets[pid]       = -1;
            game.player_status[pid] = PLAYER_LEFT;
        } else if (in.packet_type == READY) {
            game.player_status[pid] = PLAYER_ACTIVE;
            ready_count++;
        }
    }
    if (ready_count < 2) {
        server_packet_t halt = { .packet_type = HALT };
        for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
            int fd = game.sockets[pid];
            if (fd < 0) continue;
            send(fd, &halt, sizeof(halt), 0);
            close(fd);
        }
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    mkdir("logs", 0755);
    mkdir("build/logs", 0755);

    int server_fds[NUM_PORTS];
    struct sockaddr_in addr;
    int opt = 1;

    // Set up listener sockets
    for (int i = 0; i < NUM_PORTS; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(BASE_PORT + i);
        assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(listen(fd, 1) == 0);
        server_fds[i] = fd;
    }

    int seed = (argc == 2 ? atoi(argv[1]) : 0);
    init_game_state(&game, 100, seed);

    // Accept JOINs
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int cfd = accept(server_fds[pid], NULL, NULL);
        game.sockets[pid]       = cfd;
        game.player_status[pid] = PLAYER_ACTIVE;
        game.num_players++;

        client_packet_t join_pkt;
        recv(cfd, &join_pkt, sizeof(join_pkt), 0);
        assert(join_pkt.packet_type == JOIN);
    }
    for (int i = 0; i < NUM_PORTS; ++i)
        close(server_fds[i]);

    // Main game loop
    while (wait_for_ready(server_fds)) {
        reset_game_state(&game);

        // Rotate dealer
        if (game.dealer_player < 0) {
            for (int i = 0; i < MAX_PLAYERS; ++i)
                if (game.player_status[i] == PLAYER_ACTIVE) {
                    game.dealer_player = i;
                    break;
                }
        } else {
            do {
                game.dealer_player = (game.dealer_player + 1) % MAX_PLAYERS;
            } while (game.player_status[game.dealer_player] != PLAYER_ACTIVE);
        }

        // Deal and prepare pre-flop
        server_deal(&game);
        game.round_stage = ROUND_PREFLOP;
        memset(game.current_bets, 0, sizeof game.current_bets);
        game.highest_bet = 0;

        // First broadcast
        game.current_player = (game.dealer_player + 1) % MAX_PLAYERS;
        while (game.player_status[game.current_player] != PLAYER_ACTIVE)
            game.current_player = next_active_player();
        broadcast_info();

        // Four betting rounds (pre-flop, flop, turn, river)
        for (int stage = 0; stage < 4; ++stage) {
            memset(has_acted, 0, sizeof has_acted);
            if (stage > 0) {
                server_community(&game);
                memset(game.current_bets, 0, sizeof game.current_bets);
                game.highest_bet = 0;
                game.current_player = (game.dealer_player + 1) % MAX_PLAYERS;
                while (game.player_status[game.current_player] != PLAYER_ACTIVE)
                    game.current_player = next_active_player();
                broadcast_info();
            }

            int num_active = count_active_players();
            int actions = 0;
            bool short_circuit = false;

            while (actions < num_active) {
                client_packet_t in;
                recv(game.sockets[game.current_player], &in, sizeof(in), 0);
                server_packet_t resp;
                int ok = handle_client_action(&game, game.current_player, &in, &resp);
                send(game.sockets[game.current_player], &resp, sizeof(resp), 0);

                if (ok == 0) {
                    if (in.packet_type == RAISE) {
                        num_active = count_active_players();
                        actions = 1;
                    } else {
                        actions++;
                    }
                    if (count_active_players() == 1) { short_circuit = true; break; }
                    if (actions < num_active) {
                        game.current_player = next_active_player();
                        broadcast_info();
                    }
                }
            }
            if (short_circuit) break;
        }

        // Determine winner and payout
        int winner = find_winner(&game);
        game.player_stacks[winner] += game.pot_size;
        broadcast_end(winner);
    }

    // Cleanup
    printf("[Server] Shutting down.\n");
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (game.sockets[i] >= 0) close(game.sockets[i]);
    }
    return 0;
}
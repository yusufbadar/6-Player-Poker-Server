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

#define BASE_PORT       2201
#define NUM_PORTS       6
#define BUFFER_SIZE     1024

// Track who has acted each street
int has_acted[MAX_PLAYERS] = {0};
int last_raiser = -1;
// Global game state
static game_state_t game;
static int listener_fds[NUM_PORTS];

// Helper: Broadcast a packet to all connected players
static void broadcast_all(const server_packet_t *pkt) {
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int fd = game.sockets[pid];
        if (fd >= 0) send(fd, pkt, sizeof(*pkt), 0);
    }
}

static void broadcast_info(void) {
    server_packet_t pkt;
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int fd = game.sockets[pid];
        if (fd < 0) continue;
        build_info_packet(&game, pid, &pkt);
        send(fd, &pkt, sizeof(pkt), 0);
    }
}

static void broadcast_end(int winner) {
    server_packet_t pkt;
    build_end_packet(&game, winner, &pkt);
    broadcast_all(&pkt);
}

// Rotate to the next active player
static player_id_t next_active_player(void) {
    for (int i = 0, cur = game.current_player; i < MAX_PLAYERS; ++i) {
        cur = (cur + 1) % MAX_PLAYERS;
        if (game.player_status[cur] == PLAYER_ACTIVE)
            return cur;
    }
    return -1;
}

// Count still-active players
static int count_active_players(void) {
    int cnt = 0;
    for (int p = 0; p < MAX_PLAYERS; ++p)
        if (game.player_status[p] == PLAYER_ACTIVE)
            cnt++;
    return cnt;
}

// Reset per-hand state
static void start_new_hand(void) {
    shuffle_deck(game.deck);
    game.round_stage    = ROUND_INIT;
    game.next_card      = 0;
    game.highest_bet    = 0;
    game.pot_size       = 0;
    memset(game.current_bets, 0, sizeof game.current_bets);
    memset(has_acted, 0, sizeof has_acted);

    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (game.player_status[p] != PLAYER_LEFT)
            game.player_status[p] = PLAYER_ACTIVE;
        game.player_hands[p][0] = game.player_hands[p][1] = NOCARD;
    }
    for (int c = 0; c < MAX_COMMUNITY_CARDS; ++c)
        game.community_cards[c] = NOCARD;
}

// Wait until at least two players send READY, otherwise HALT
static bool wait_for_ready(void) {
    int ready_count = 0;
    client_packet_t in;

    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int fd = game.sockets[pid];
        if (fd < 0) continue;
        int n = recv(fd, &in, sizeof(in), 0);
        if (n <= 0 || in.packet_type == LEAVE) {
            server_packet_t ack = { .packet_type = ACK };
            send(fd, &ack, sizeof(ack), 0);
            close(fd);
            game.sockets[pid]       = -1;
            game.player_status[pid] = PLAYER_LEFT;
            continue;
        }
        if (in.packet_type == READY) {
            game.player_status[pid] = PLAYER_ACTIVE;
            ready_count++;
        }
    }
    if (ready_count < 2) {
        server_packet_t halt = { .packet_type = HALT };
        broadcast_all(&halt);
        return false;
    }
    return true;
}

static void deal_and_bet_cycle(void) {
    // Deal hole cards & pre-flop
    server_deal(&game);
    game.round_stage = ROUND_PREFLOP;
    game.current_player = next_active_player();
    broadcast_info();

    // Four streets: flop, turn, river handled via loop
    for (int stage = 0; stage < 4; ++stage) {
        if (stage > 0) {
            server_community(&game);
            broadcast_info();
        }
        memset(game.current_bets, 0, sizeof game.current_bets);
        game.highest_bet = 0;
        memset(has_acted, 0, sizeof has_acted);

        int to_act = count_active_players();
        int acted = 0;
        bool end_round = false;

        while (acted < to_act && !end_round) {
            int pid = game.current_player;
            client_packet_t in;
            recv(game.sockets[pid], &in, sizeof(in), 0);
            server_packet_t resp;
            int ok = handle_client_action(&game, pid, &in, &resp);
            send(game.sockets[pid], &resp, sizeof(resp), 0);

            if (ok == 0) {
                if (in.packet_type == RAISE) {
                    to_act = count_active_players();
                    acted = 1;
                } else {
                    acted++;
                }
                if (count_active_players() == 1) {
                    end_round = true;
                    break;
                }
                game.current_player = next_active_player();
                broadcast_info();
            }
        }
        if (end_round) break;
    }
}

int main(int argc, char **argv) {
    // Prepare logs
    mkdir("logs", 0755);
    mkdir("build/logs", 0755);

    // Open listening sockets
    struct sockaddr_in addr;
    int opt = 1;
    for (int i = 0; i < NUM_PORTS; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(BASE_PORT + i);
        assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(listen(fd, 1) == 0);
        listener_fds[i] = fd;
    }

    // Initialize game state
    int seed = (argc == 2 ? atoi(argv[1]) : 0);
    init_game_state(&game, MAX_PLAYERS, seed);

    // Accept player connections
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int cfd = accept(listener_fds[pid], NULL, NULL);
        game.sockets[pid] = cfd;
        game.player_status[pid] = PLAYER_ACTIVE;
        game.num_players++;
        client_packet_t join;
        recv(cfd, &join, sizeof(join), 0);
        assert(join.packet_type == JOIN);
    }
    for (int i = 0; i < NUM_PORTS; ++i)
        close(listener_fds[i]);

    // Main game loop
    while (wait_for_ready()) {
        start_new_hand();

        // Rotate dealer
        int d = game.dealer_player;
        do { d = (d < 0 ? 0 : (d + 1) % MAX_PLAYERS); }
        while (game.player_status[d] != PLAYER_ACTIVE);
        game.dealer_player = d;

        deal_and_bet_cycle();

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

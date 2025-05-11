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

#define BASE_PORT     2201
#define NUM_PORTS     6
#define BUFFER_SIZE   1024

// Track who has acted each street
extern int has_acted[MAX_PLAYERS];
// Global game state
game_state_t game;

// Reset deck, bets, hands, community cards
void reset_game_state(game_state_t *g) {
    shuffle_deck(g->deck);
    g->round_stage    = ROUND_INIT;
    g->next_card      = 0;
    g->highest_bet    = 0;
    g->pot_size       = 0;
    
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (g->player_status[p] != PLAYER_LEFT)
            g->player_status[p] = PLAYER_ACTIVE;
        g->current_bets[p] = 0;
        g->player_hands[p][0] = g->player_hands[p][1] = NOCARD;
    }
    for (int c = 0; c < MAX_COMMUNITY_CARDS; ++c)
        g->community_cards[c] = NOCARD;
}

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

// Broadcast INFO packet to all
static void broadcast_info(void) {
    server_packet_t pkt;
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int fd = game.sockets[pid];
        if (fd < 0) continue;
        build_info_packet(&game, pid, &pkt);
        send(fd, &pkt, sizeof(pkt), 0);
    }
}

// Broadcast END packet
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
    int n = 0;
    for (int p = 0; p < MAX_PLAYERS; ++p)
        if (game.player_status[p] == PLAYER_ACTIVE)
            ++n;
    return n;
}

int main(int argc, char **argv) {
    // Prepare log directories
    mkdir("logs", 0755);
    mkdir("build/logs", 0755);

    int server_fds[NUM_PORTS];
    struct sockaddr_in addr;
    int opt = 1;

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

    while (1) {
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
                continue;
            }
            if (in.packet_type == READY) {
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
            break;
        }

        // Start a new hand
        reset_game_state(&game);

        // Rotate dealer
        if (game.dealer_player < 0) {
            for (int i = 0; i < MAX_PLAYERS; ++i) {
                if (game.player_status[i] == PLAYER_ACTIVE) {
                    game.dealer_player = i;
                    break;
                }
            }
        } else {
            do {
                game.dealer_player = (game.dealer_player + 1) % MAX_PLAYERS;
            } while (game.player_status[game.dealer_player] != PLAYER_ACTIVE);
        }

        // Deal hole cards
        server_deal(&game);
        
        // Initialize betting pre-flop
        game.round_stage = ROUND_PREFLOP;
        memset(game.current_bets, 0, sizeof game.current_bets);
        game.highest_bet = 0;

        // First broadcast of pre-flop state
        
        game.current_player = (game.dealer_player + 1) % MAX_PLAYERS;
        while (game.player_status[game.current_player] != PLAYER_ACTIVE) {
            game.current_player = (game.current_player + 1) % MAX_PLAYERS;}
        broadcast_info();

        for (int stage = 0; stage < 4; ++stage) {
            memset(has_acted, 0, sizeof has_acted);

            if (stage > 0) {
                server_community(&game);
            }

            // Reset bets for this street
            memset(game.current_bets, 0, sizeof game.current_bets);
            game.highest_bet = 0;

            // Pick first to act (left of dealer)
            game.current_player = (game.dealer_player + 1) % MAX_PLAYERS;
            while (game.player_status[game.current_player] != PLAYER_ACTIVE)
                game.current_player = next_active_player();

            int num_active = count_active_players();
            int actions    = 0;
            bool short_circuit = false;

            while (actions < num_active) {
                client_packet_t in;
                recv(game.sockets[game.current_player], &in, sizeof(in), 0);

                server_packet_t resp;
                int ok = handle_client_action(&game, game.current_player, &in, &resp);
                send(game.sockets[game.current_player], &resp, sizeof(resp), 0);

                if (ok == 0) {
                    if (in.packet_type == RAISE) {
                        // Everyone must act again
                        memset(has_acted, 0, sizeof has_acted);
                        has_acted[game.current_player] = 1;
                        actions = 1;
                    } else {
                        has_acted[game.current_player] = 1;
                        actions++;
                    }

                    broadcast_info();

                    // Check for single remaining player → short-circuit
                    if (count_active_players() == 1) {
                        short_circuit = true;
                        break;
                    }

                    // Next to act if more actions remain
                    if (actions < num_active) {
                        game.current_player = next_active_player();
                    }
                }
            }
            if (short_circuit) break;
        }

        int winner = (count_active_players() == 1)
            ? (find_winner(&game))
            : find_winner(&game);
        game.player_stacks[winner] += game.pot_size;
        broadcast_end(winner);
    }

    // Final cleanup of any remaining fds
    printf("[Server] Shutting down.\n");
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        if (game.player_status[pid] != PLAYER_LEFT)
            close(game.sockets[pid]);
    }

    return 0;
}
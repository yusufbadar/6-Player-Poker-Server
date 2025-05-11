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
int has_acted[MAX_PLAYERS] = {0};
int last_raiser = -1;
// Global game state
game_state_t game;

// Reset deck, bets, hands, community cards
void reset_game_state(game_state_t *g) {
    shuffle_deck(g->deck);
    g->round_stage = ROUND_INIT;
    g->next_card = 0;
    g->highest_bet = 0;
    g->pot_size = 0;

    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (g->player_status[p] != PLAYER_LEFT)
            g->player_status[p] = PLAYER_ACTIVE;
        g->current_bets[p] = 0;
        g->player_hands[p][0] = g->player_hands[p][1] = NOCARD;
    }
    for (int c = 0; c < MAX_COMMUNITY_CARDS; ++c)
        g->community_cards[c] = NOCARD;
}

// Find the next active player id
static player_id_t next_active_player(void) {
    player_id_t pid = game.current_player;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        pid = (pid + 1) % MAX_PLAYERS;
        if (game.player_status[pid] == PLAYER_ACTIVE)
            return pid;
    }
    return -1;
}

// Send packet to all connected clients
static void send_to_all(server_packet_t *pkt) {
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int sock = game.sockets[pid];
        if (sock >= 0)
            send(sock, pkt, sizeof(*pkt), 0);
    }
}

// Broadcast INFO and END packets
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
    send_to_all(&pkt);
}

// How many players are still active?
static int count_active_players(void) {
    int total = 0;
    for (int p = 0; p < MAX_PLAYERS; ++p)
        if (game.player_status[p] == PLAYER_ACTIVE)
            ++total;
    return total;
}

// Initialize listening sockets
static void init_listeners(int fds[NUM_PORTS]) {
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
        fds[i] = fd;
    }
}

// Accept join requests from clients
static void accept_players(int fds[NUM_PORTS]) {
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int client_fd = accept(fds[pid], NULL, NULL);
        game.sockets[pid] = client_fd;
        game.player_status[pid] = PLAYER_ACTIVE;
        game.num_players++;

        client_packet_t join_pkt;
        recv(client_fd, &join_pkt, sizeof(join_pkt), 0);
        assert(join_pkt.packet_type == JOIN);
    }
    for (int i = 0; i < NUM_PORTS; ++i)
        close(fds[i]);
}

// Main game loop handling betting rounds and community deals
static void run_game(void) {
    while (true) {
        int ready = 0;
        for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
            int s = game.sockets[pid];
            if (s < 0) continue;

            client_packet_t in;
            int n = recv(s, &in, sizeof(in), 0);
            if (n <= 0 || in.packet_type == LEAVE) {
                server_packet_t ack = { .packet_type = ACK };
                send(s, &ack, sizeof(ack), 0);
                close(s);
                game.sockets[pid] = -1;
                game.player_status[pid] = PLAYER_LEFT;
                continue;
            }
            if (in.packet_type == READY) {
                game.player_status[pid] = PLAYER_ACTIVE;
                ready++;
            }
        }
        if (ready < 2) {
            server_packet_t halt = { .packet_type = HALT };
            send_to_all(&halt);
            break;
        }

        reset_game_state(&game);

        // Determine dealer
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

        // Deal hole cards and initial broadcast
        server_deal(&game);
        game.round_stage = ROUND_PREFLOP;
        memset(game.current_bets, 0, sizeof game.current_bets);
        game.highest_bet = 0;
        game.current_player = (game.dealer_player + 1) % MAX_PLAYERS;
        while (game.player_status[game.current_player] != PLAYER_ACTIVE)
            game.current_player = next_active_player();
        broadcast_info();

        // Betting and community phases
        for (int stage = 0; stage < 4; ++stage) {
            memset(has_acted, 0, sizeof has_acted);
            memset(game.current_bets, 0, sizeof game.current_bets);
            game.highest_bet = 0;

            game.current_player = (game.dealer_player + 1) % MAX_PLAYERS;
            while (game.player_status[game.current_player] != PLAYER_ACTIVE)
                game.current_player = next_active_player();

            int active_count = count_active_players();
            int actions = 0;
            bool done = false;

            while (actions < active_count) {
                client_packet_t in;
                recv(game.sockets[game.current_player], &in, sizeof(in), 0);
                server_packet_t resp;
                int ok = handle_client_action(&game, game.current_player, &in, &resp);
                send(game.sockets[game.current_player], &resp, sizeof(resp), 0);

                if (ok == 0) {
                    if (in.packet_type == RAISE) {
                        active_count = count_active_players();
                        actions = 1;
                    } else {
                        actions++;
                    }
                    if (count_active_players() == 1) {
                        done = true;
                        break;
                    }
                    if (actions < active_count) {
                        game.current_player = next_active_player();
                        broadcast_info();
                    }
                }
            }
            if (done) break;

            if (stage < 3) {
                server_community(&game);
                broadcast_info();
            }
        }

        int winner = find_winner(&game);
        game.player_stacks[winner] += game.pot_size;
        broadcast_end(winner);
    }
}

int main(int argc, char **argv) {
    mkdir("logs", 0755);
    mkdir("build/logs", 0755);

    int listeners[NUM_PORTS];
    init_listeners(listeners);

    int seed = (argc == 2 ? atoi(argv[1]) : 0);
    init_game_state(&game, 100, seed);

    accept_players(listeners);
    run_game();

    printf("[Server] Shutting down.\n");
    for (int i = 0; i < NUM_PORTS; ++i)
        close(listeners[i]);
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (game.player_status[p] != PLAYER_LEFT)
            close(game.sockets[p]);
    }

    return 0;
}

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>

#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"

#define BASE_PORT   2201
#define NUM_PORTS   6
#define BUFFER_SIZE 1024

int has_acted[MAX_PLAYERS] = {0};  // actions per street tracker
int last_raiser = -1;             // most recent raiser

game_state_t game;               // single-instance game state

static void clear_game(game_state_t *g) {
    shuffle_deck(g->deck);
    g->round_stage = ROUND_INIT;
    g->next_card   = 0;
    g->highest_bet = 0;
    g->pot_size    = 0;

    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (g->player_status[i] != PLAYER_LEFT) {
            g->player_status[i] = PLAYER_ACTIVE;
        }
        g->current_bets[i]     = 0;
        g->player_hands[i][0]  = NOCARD;
        g->player_hands[i][1]  = NOCARD;
    }

    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i) {
        g->community_cards[i] = NOCARD;
    }
}

static player_id_t next_active_player(void) {
    player_id_t candidate = game.current_player;
    for (int steps = 0; steps < MAX_PLAYERS; ++steps) {
        candidate = (candidate + 1) % MAX_PLAYERS;
        if (game.player_status[candidate] == PLAYER_ACTIVE) {
            return candidate;
        }
    }
    return -1;
}

static void broadcast_to_all(server_packet_t *packet) {
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int sock = game.sockets[pid];
        if (sock >= 0) {
            send(sock, packet, sizeof(*packet), 0);
        }
    }
}

static void broadcast_info(void) {
    server_packet_t pkt;
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int sock = game.sockets[pid];
        if (sock < 0) continue;
        build_info_packet(&game, pid, &pkt);
        send(sock, &pkt, sizeof(pkt), 0);
    }
}

static void broadcast_end(int winner) {
    server_packet_t pkt;
    build_end_packet(&game, winner, &pkt);
    broadcast_to_all(&pkt);
}

static int count_active(void) {
    int cnt = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (game.player_status[i] == PLAYER_ACTIVE) {
            ++cnt;
        }
    }
    return cnt;
}

int main(int argc, char *argv[]) {
    mkdir("logs", 0755);
    mkdir("build/logs", 0755);

    int server_fds[NUM_PORTS];
    struct sockaddr_in addr;
    int on = 1;

    for (int i = 0; i < NUM_PORTS; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

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

    // accept JOIN from each position
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int cfd = accept(server_fds[pid], NULL, NULL);
        game.sockets[pid]       = cfd;
        game.player_status[pid] = PLAYER_ACTIVE;
        game.num_players++;

        client_packet_t join;
        recv(cfd, &join, sizeof(join), 0);
        assert(join.packet_type == JOIN);
    }
    // close listeners
    for (int i = 0; i < NUM_PORTS; ++i) {
        close(server_fds[i]);
    }

    while (1) {
        int ready_count = 0;
        for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
            int sock = game.sockets[pid];
            if (sock < 0) continue;

            client_packet_t in;
            int r = recv(sock, &in, sizeof(in), 0);
            if (r <= 0 || in.packet_type == LEAVE) {
                server_packet_t ack = { .packet_type = ACK };
                send(sock, &ack, sizeof(ack), 0);
                close(sock);
                game.sockets[pid]       = -1;
                game.player_status[pid] = PLAYER_LEFT;
            } else if (in.packet_type == READY) {
                game.player_status[pid] = PLAYER_ACTIVE;
                ++ready_count;
            }
        }

        if (ready_count < 2) {
            server_packet_t halt = { .packet_type = HALT };
            broadcast_to_all(&halt);
            break;
        }

        clear_game(&game);

        // dealer rotation
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

        server_deal(&game);
        game.round_stage = ROUND_PREFLOP;
        memset(game.current_bets, 0, sizeof game.current_bets);
        game.highest_bet = 0;

        // first turn
        game.current_player = (game.dealer_player + 1) % MAX_PLAYERS;
        while (game.player_status[game.current_player] != PLAYER_ACTIVE) {
            game.current_player = next_active_player();
        }
        broadcast_info();

        // four betting rounds
        for (int stage = 0; stage < 4; ++stage) {
            memset(has_acted, 0, sizeof has_acted);
            memset(game.current_bets, 0, sizeof game.current_bets);
            game.highest_bet = 0;

            game.current_player = (game.dealer_player + 1) % MAX_PLAYERS;
            while (game.player_status[game.current_player] != PLAYER_ACTIVE) {
                game.current_player = next_active_player();
            }

            int active = count_active();
            int acts = 0;
            bool done = false;

            while (acts < active) {
                client_packet_t in;
                recv(game.sockets[game.current_player], &in, sizeof(in), 0);
                server_packet_t out;
                int ok = handle_client_action(&game, game.current_player, &in, &out);
                send(game.sockets[game.current_player], &out, sizeof(out), 0);

                if (ok == 0) {
                    if (in.packet_type == RAISE) {
                        active = count_active();
                        acts = 1;
                    } else {
                        ++acts;
                    }
                    if (count_active() == 1) {
                        done = true;
                        break;
                    }
                    if (acts < active) {
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

    printf("[Server] Shutting down.\n");
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (game.player_status[p] != PLAYER_LEFT) {
            close(game.sockets[p]);
        }
    }

    return 0;
}

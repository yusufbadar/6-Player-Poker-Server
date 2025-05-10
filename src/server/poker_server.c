#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <assert.h>
#include <stdbool.h>

#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"

#define BASE_PORT   2201
#define NUM_PORTS   6
#define BUFFER_SIZE 1024

typedef struct {
    int socket;
    struct sockaddr_in address;
} player_t;

extern int has_acted[MAX_PLAYERS];

static int server_fds[NUM_PORTS];
static game_state_t game;

static void reset_game_state_local(game_state_t *g) {
    shuffle_deck(g->deck);
    g->round_stage = ROUND_INIT;
    g->next_card   = 0;
    g->highest_bet = 0;
    g->pot_size    = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (g->player_status[i] != PLAYER_LEFT) {
            g->player_status[i] = PLAYER_ACTIVE;
        }
        g->current_bets[i] = 0;
        g->player_hands[i][0] = g->player_hands[i][1] = NOCARD;
    }
    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i) {
        g->community_cards[i] = NOCARD;
    }
}

static void reset_for_new_street(game_state_t *g) {
    g->highest_bet = 0;
    memset(g->current_bets, 0, sizeof g->current_bets);
    memset(has_acted, 0, sizeof has_acted);
}

static player_id_t next_active_global(void) {
    player_id_t curr = game.current_player;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        curr = (curr + 1) % MAX_PLAYERS;
        if (game.player_status[curr] == PLAYER_ACTIVE) {
            return curr;
        }
    }
    return (player_id_t)-1;
}

static void broadcast_info(void) {
    server_packet_t pkt;
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int fd = game.sockets[pid];
        if (fd < 0) continue;
        build_info_packet(&game, pid, &pkt);
        send(fd, &pkt, sizeof pkt, 0);
    }
}

static void broadcast_end(int winner) {
    server_packet_t pkt;
    build_end_packet(&game, winner, &pkt);
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int fd = game.sockets[pid];
        if (fd < 0) continue;
        send(fd, &pkt, sizeof pkt, 0);
    }
}

static int count_active_players(void) {
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (game.player_status[i] == PLAYER_ACTIVE
         || game.player_status[i] == PLAYER_ALLIN) {
            ++n;
        }
    }
    return n;
}

int main(int argc, char **argv) {
    mkdir("logs", 0755);

    for (int i = 0; i < NUM_PORTS; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
        struct sockaddr_in addr = {0};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(BASE_PORT + i);
        assert(bind(fd, (struct sockaddr*)&addr, sizeof addr) == 0);
        assert(listen(fd, 1) == 0);
        server_fds[i] = fd;
    }

    int seed = (argc == 2) ? atoi(argv[1]) : 0;
    init_game_state(&game, 100, seed);

    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int cfd = accept(server_fds[pid], NULL, NULL);
        game.sockets[pid]       = cfd;
        game.player_status[pid] = PLAYER_ACTIVE;
        game.num_players++;
        client_packet_t join_pkt;
        recv(cfd, &join_pkt, sizeof join_pkt, 0);
        assert(join_pkt.packet_type == JOIN);
        server_packet_t ack = { .packet_type = ACK };
        send(cfd, &ack, sizeof ack, 0);
    }
    for (int i = 0; i < NUM_PORTS; ++i) close(server_fds[i]);

    while (1) {
        int ready_count = 0;
        for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
            int fd = game.sockets[pid];
            if (fd < 0) continue;
            client_packet_t in;
            int n = recv(fd, &in, sizeof in, 0);
            if (n <= 0 || in.packet_type == LEAVE) {
                server_packet_t ack = { .packet_type = ACK };
                send(fd, &ack, sizeof ack, 0);
                close(fd);
                game.sockets[pid] = -1;
                game.player_status[pid] = PLAYER_LEFT;
            } else if (in.packet_type == READY) {
                game.player_status[pid] = PLAYER_ACTIVE;
                ++ready_count;
            }
        }
        if (ready_count < 2) {
            server_packet_t halt = { .packet_type = HALT };
            for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
                int fd = game.sockets[pid];
                if (fd < 0) continue;
                send(fd, &halt, sizeof halt, 0);
                close(fd);
            }
            break;
        }

        reset_game_state(&game);
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
        game.round_stage    = ROUND_PREFLOP;
        game.highest_bet    = 0;
        memset(game.current_bets, 0, sizeof game.current_bets);
        game.current_player = next_active_global();

        broadcast_info();

        for (int stage = 0; stage < 4; ++stage) {
            int num_active = count_active_players();
            int actions    = 0;
            bool done      = false;
            while (actions < num_active && !done) {
                player_id_t pid = game.current_player;
                client_packet_t in;
                recv(game.sockets[pid], &in, sizeof in, 0);
                server_packet_t resp;
                int ok = handle_client_action(&game, pid, &in, &resp);
                send(game.sockets[pid], &resp, sizeof resp, 0);

                if (ok == 0 && resp.packet_type == ACK) {
                    has_acted[pid] = 1;

                    if (in.packet_type == RAISE) {
                        for (int i = 0; i < MAX_PLAYERS; ++i) {
                            has_acted[i] = (i == pid) ? 1 : 0;
                        }
                        num_active = count_active_players();
                        actions    = 1;
                    } else {
                        ++actions;
                    }

                    int live = count_active_players();
                    if (live <= 1) {
                        done = true;
                    } else if (actions < num_active) {
                        game.current_player = next_active_global();
                        broadcast_info();
                    } else if (stage < 3) {
                        server_community(&game);
                        reset_for_new_street(&game);
                        game.current_player = next_active_global();
                        broadcast_info();
                    }
                }
            }
            if (count_active_players() <= 1) break;
        }

        int winner = find_winner(&game);
        if (winner >= 0) game.player_stacks[winner] += game.pot_size;
        broadcast_end(winner);
    }

    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        if (game.player_status[pid] != PLAYER_LEFT) {
            close(game.sockets[pid]);
        }
    }
    printf("[Server] Shutting down.\n");
    return 0;
}

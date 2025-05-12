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

#define BASE_PORT 2201
#define NUM_PORTS 6
#define BUFFER_SIZE 1024

#define EACH_PLAYER for (int pid = 0; pid < MAX_PLAYERS; ++pid)
#define NEXT(i) ((i + 1) % MAX_PLAYERS)

static int street_has_acted[MAX_PLAYERS] = {0};
static int last_raiser = -1;

static game_state_t game;

static inline void send_pkt(int pid, const server_packet_t *pkt)
{
    int fd = game.sockets[pid];
    if (fd >= 0) send(fd, pkt, sizeof(*pkt), 0);
}

static int count_active_players(void)
{
    int n = 0;
    EACH_PLAYER if (game.player_status[pid] == PLAYER_ACTIVE) ++n;
    return n;
}

void reset_game_state(game_state_t *gs) {
    shuffle_deck(gs->deck);

    gs->round_stage = ROUND_INIT;
    gs->next_card = 0;
    gs->highest_bet = 0;
    gs->pot_size = 0;

    memset(gs->current_bets, 0, sizeof gs->current_bets);

    for (player_id_t p = 0; p < MAX_PLAYERS; ++p) {
        if (gs->player_status[p] != PLAYER_LEFT) {
            gs->player_status[p] = PLAYER_ACTIVE;
        }

        gs->player_hands[p][0] = NOCARD;
        gs->player_hands[p][1] = NOCARD;
    }

    memset(gs->community_cards, NOCARD, sizeof gs->community_cards);
}

static player_id_t next_active_player(player_id_t start)
{
    int cur = start;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        cur = NEXT(cur);
        if (game.player_status[cur] == PLAYER_ACTIVE) return cur;
    }
    return -1;
}

static void broadcast_info(void)
{
    server_packet_t pkt;
    EACH_PLAYER {
        if (game.sockets[pid] < 0) continue;
        build_info_packet(&game, pid, &pkt);
        send_pkt(pid, &pkt);
    }
}

static void broadcast_end(int winner)
{
    server_packet_t pkt;
    build_end_packet(&game, winner, &pkt);
    EACH_PLAYER send_pkt(pid, &pkt);
}

static void accept_and_confirm(int listen_fd, player_id_t id) {
    int cli_fd = accept(listen_fd, NULL, NULL);
    assert(cli_fd >= 0);

    game.sockets[id]      = cli_fd;
    game.player_status[id]= PLAYER_ACTIVE;
    ++game.num_players;

    client_packet_t first_msg;
    ssize_t bytes = recv(cli_fd, &first_msg, sizeof first_msg, 0);
    assert(bytes == sizeof first_msg && first_msg.packet_type == JOIN);
}

int main(int argc, char **argv)
{

    int server_fds[NUM_PORTS] = {0};
    struct sockaddr_in addr = {0};
    int opt = 1;

    for (int i = 0; i < NUM_PORTS; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(BASE_PORT + i);
        assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(listen(fd, 1) == 0);
        server_fds[i] = fd;
    }

    int seed = (argc == 2) ? atoi(argv[1]) : 0;
    init_game_state(&game, 100, seed);

    for (player_id_t id = 0; id < MAX_PLAYERS; ++id) {
        accept_and_confirm(server_fds[id], id);
    }
    for (int i = 0; i < NUM_PORTS; ++i) {
        close(server_fds[i]);
    } 
    while (1) {
        int ready_cnt = 0;
        EACH_PLAYER {
            int fd = game.sockets[pid];
            if (fd < 0) continue;
            client_packet_t in;
            int n = recv(fd, &in, sizeof(in), 0);
            if (n <= 0 || in.packet_type == LEAVE) {
                server_packet_t ack = { .packet_type = ACK };
                send_pkt(pid, &ack);
                close(fd);

                game.sockets[pid] = -1;
                game.player_status[pid] = PLAYER_LEFT;
                continue;
            }
            if (in.packet_type == READY) {
                game.player_status[pid] = PLAYER_ACTIVE;
                ++ready_cnt;
            }
        }
        if (ready_cnt < 2) {
            server_packet_t halt = { .packet_type = HALT };
            EACH_PLAYER {
                send_pkt(pid, &halt);
                if (game.sockets[pid] >= 0) close(game.sockets[pid]);
            }
            break;
        }

        reset_game_state(&game);

        if (game.dealer_player < 0) {
            EACH_PLAYER if (game.player_status[pid] == PLAYER_ACTIVE) {
                game.dealer_player = pid;
                break;
            }
        } else {
            do {
                game.dealer_player = NEXT(game.dealer_player);
            } while (game.player_status[game.dealer_player] != PLAYER_ACTIVE);
        }

        server_deal(&game);
        game.round_stage = ROUND_PREFLOP;
        memset(game.current_bets, 0, sizeof game.current_bets);
        game.highest_bet = 0;
        game.current_player = NEXT(game.dealer_player);
        while (game.player_status[game.current_player] != PLAYER_ACTIVE) {
            game.current_player = NEXT(game.current_player);
        }
        broadcast_info();
        int st = 0;
        bool hand_finished = false;

        while (st < 4 && !hand_finished) {
            memset(street_has_acted, 0, sizeof street_has_acted);
            memset(game.current_bets, 0, sizeof game.current_bets);
            game.highest_bet = 0;

            game.current_player = NEXT(game.dealer_player);
            while (game.player_status[game.current_player] != PLAYER_ACTIVE) {
                game.current_player = NEXT(game.current_player);
            }

            int todo = count_active_players();
            int acted = 0;

            while (acted < todo) {
                client_packet_t cli;
                recv(game.sockets[game.current_player], &cli, sizeof cli, 0);

                server_packet_t srv;
                int valid = handle_client_action(&game, game.current_player, &cli, &srv);
                send_pkt(game.current_player, &srv);

                if (valid != 0) {
                    continue;
                }
                if (cli.packet_type == RAISE) {
                    todo  = count_active_players();
                    acted = 1;
                } else {
                    ++acted;
                }
                if (count_active_players() == 1) {
                    hand_finished = true;
                    break;
                }
                if (acted < todo) {
                    game.current_player = NEXT(game.current_player);
                    while (game.player_status[game.current_player] != PLAYER_ACTIVE) {
                        game.current_player = NEXT(game.current_player);
                    }
                    broadcast_info();
                }
            }

            if (hand_finished) {
                break;
            }

            if (st < 3) {
                server_community(&game);
                memset(game.current_bets, 0, sizeof game.current_bets);
                game.highest_bet  = 0;

                game.current_player = NEXT(game.dealer_player);
                while (game.player_status[game.current_player] != PLAYER_ACTIVE) {
                    game.current_player = NEXT(game.current_player);
                }
                broadcast_info();
            }
            ++st;
        }

        int winner = find_winner(&game);
        game.player_stacks[winner] += game.pot_size;
        broadcast_end(winner);
    }

    puts("[Server] Shutting down.");
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        close(server_fds[i]);
        if (game.player_status[i] != PLAYER_LEFT) {
            close(game.sockets[i]);
        }
    }
    return 0;
}
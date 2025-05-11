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
    int               socket;
    struct sockaddr_in address;
} player_t;

game_state_t game;
static int   listen_fds[NUM_PORTS]; 

void reset_game_state(game_state_t *g)
{
    shuffle_deck(g->deck);
    g->round_stage = ROUND_INIT;
    g->next_card   = 0;
    g->highest_bet = 0;
    g->pot_size    = 0;

    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (g->player_status[p] != PLAYER_LEFT)
            g->player_status[p] = PLAYER_ACTIVE;
        g->current_bets [p] = 0;
        g->player_hands[p][0] = g->player_hands[p][1] = NOCARD;
    }
    for (int c = 0; c < MAX_COMMUNITY_CARDS; ++c)
        g->community_cards[c] = NOCARD;
}

static player_id_t next_active_player(void)
{
    int cur = game.current_player;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        cur = (cur + 1) % MAX_PLAYERS;
        if (game.player_status[cur] == PLAYER_ACTIVE)
            return cur;
    }
    return -1;
}

static void broadcast_info(void)
{
    server_packet_t pkt;
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        int fd = game.sockets[p];
        if (fd < 0) continue;
        build_info_packet(&game, p, &pkt);
        send(fd, &pkt, sizeof(pkt), 0);
    }
}

static void broadcast_end(int winner)
{
    server_packet_t pkt;
    build_end_packet(&game, winner, &pkt);
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        int fd = game.sockets[p];
        if (fd < 0) continue;
        send(fd, &pkt, sizeof(pkt), 0);
    }
}

static int count_active_players(void)
{
    int n = 0;
    for (int p = 0; p < MAX_PLAYERS; ++p)
        if (game.player_status[p] == PLAYER_ACTIVE)
            ++n;
    return n;
}

int main(int argc, char **argv)
{
    mkdir("logs", 0755);
    mkdir("build/logs", 0755);

    for (int i = 0; i < NUM_PORTS; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

        struct sockaddr_in addr = {0};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(BASE_PORT + i);

        assert(bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
        assert(listen(fd, 1) == 0);
        listen_fds[i] = fd;
    }

    int seed = (argc == 2) ? atoi(argv[1]) : 0;
    init_game_state(&game, 100, seed);

    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int cfd = accept(listen_fds[pid], NULL, NULL);
        game.sockets[pid]      = cfd;
        game.player_status[pid]= PLAYER_ACTIVE;
        game.num_players++;

        client_packet_t join_pkt;
        recv(cfd, &join_pkt, sizeof join_pkt, 0);
        assert(join_pkt.packet_type == JOIN);
    }
    for (int i = 0; i < NUM_PORTS; ++i) close(listen_fds[i]);

    while (1) {
        int ready = 0;
        for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
            int fd = game.sockets[pid];
            if (fd < 0) continue;

            client_packet_t in;
            int n = recv(fd, &in, sizeof in, 0);
            if (n <= 0 || in.packet_type == LEAVE) {
                close(fd);
                game.sockets[pid]      = -1;
                game.player_status[pid]= PLAYER_LEFT;
            } else if (in.packet_type == READY) {
                game.player_status[pid]= PLAYER_ACTIVE;
                ++ready;
            }
        }
        if (ready < 2) break;

        reset_game_state(&game);

        if (game.dealer_player < 0) {
            for (int p = 0; p < MAX_PLAYERS; ++p)
                if (game.player_status[p] == PLAYER_ACTIVE) {
                    game.dealer_player = p; break;
                }
        } else {
            do {
                game.dealer_player = (game.dealer_player + 1) % MAX_PLAYERS;
            } while (game.player_status[game.dealer_player] != PLAYER_ACTIVE);
        }

        server_deal(&game);

        game.current_player = (game.dealer_player + 1) % MAX_PLAYERS;
        while (game.player_status[game.current_player] != PLAYER_ACTIVE)
            game.current_player = (game.current_player + 1) % MAX_PLAYERS;

        broadcast_info();

        bool short_circuit = false;

        for (int stage = 0; stage < 4 && !short_circuit; ++stage) {

            memset(game.current_bets, 0, sizeof game.current_bets);
            game.highest_bet = 0;

            game.current_player = (game.dealer_player + 1) % MAX_PLAYERS;
            while (game.player_status[game.current_player] != PLAYER_ACTIVE)
                game.current_player = (game.current_player + 1) % MAX_PLAYERS;

            int num_active = count_active_players();
            int actions    = 0;

            while (actions < num_active) {
                client_packet_t in;
                recv(game.sockets[game.current_player], &in, sizeof in, 0);

                server_packet_t resp;
                int ok = handle_client_action(&game, game.current_player,
                                              &in, &resp);
                send(game.sockets[game.current_player], &resp, sizeof resp, 0);

                if (ok == 0) {
                    if (in.packet_type == RAISE) {
                        num_active = count_active_players();
                        actions    = 1;
                    } else {
                        ++actions;
                    }

                    if (count_active_players() == 1) { short_circuit = true; break; }

                    if (actions < num_active) {
                        game.current_player = next_active_player();
                        broadcast_info();
                    }
                    else if (stage < 3) {
                        server_community(&game);
                        memset(game.current_bets, 0, sizeof game.current_bets);
                        game.highest_bet = 0;

                        game.current_player = game.dealer_player;
                        game.current_player = next_active_player();
                        while (game.player_status[game.current_player] != PLAYER_ACTIVE)
                            game.current_player = next_active_player();

                        broadcast_info();
                    }
                }
            }
        }

        int winner;
        if (count_active_players() == 1) {
            for (int p = 0; p < MAX_PLAYERS; ++p)
                if (game.player_status[p] == PLAYER_ACTIVE) { winner = p; break; }
        } else {
            winner = find_winner(&game);
        }
        game.player_stacks[winner] += game.pot_size;
        broadcast_end(winner);
    }

    for (int p = 0; p < MAX_PLAYERS; ++p)
        if (game.player_status[p] != PLAYER_LEFT)
            close(game.sockets[p]);
    return 0;
}
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

#define BASE_PORT 2201
#define NUM_PORTS 6
#define BUFFER_SIZE 1024

static int server_fds[NUM_PORTS];
static game_state_t game;

static void broadcast_info() {
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
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int fd = game.sockets[pid];
        if (fd < 0) continue;
        send(fd, &pkt, sizeof(pkt), 0);
    }
}

static int count_active_players() {
    int n = 0;
    for (int p = 0; p < MAX_PLAYERS; ++p)
        if (game.player_status[p] == PLAYER_ACTIVE)
            ++n;
    return n;
}

int main(int argc, char **argv) {
    mkdir("logs", 0755);
    mkdir("build/logs", 0755);

    for (int i = 0; i < NUM_PORTS; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in server_address = {0};
        server_address.sin_family = AF_INET;
        server_address.sin_addr.s_addr = INADDR_ANY;
        server_address.sin_port = htons(BASE_PORT + i);

        assert(bind(fd, (struct sockaddr *)&server_address, sizeof(server_address)) == 0);
        assert(listen(fd, 1) == 0);

        server_fds[i] = fd;
    }

    int seed = (argc == 2) ? atoi(argv[1]) : 0;
    init_game_state(&game, 100, seed);

    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int cfd = accept(server_fds[pid], NULL, NULL);
        game.sockets[pid] = cfd;
        game.player_status[pid] = PLAYER_ACTIVE;
        game.num_players++;

        client_packet_t join_pkt;
        recv(cfd, &join_pkt, sizeof(join_pkt), 0);
        assert(join_pkt.packet_type == JOIN);

        server_packet_t ack = { .packet_type = ACK };
        send(cfd, &ack, sizeof(ack), 0);
    }

    for (int i = 0; i < NUM_PORTS; ++i) close(server_fds[i]);

    while (1) {
        int ready_count = 0;
        for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
            int fd = game.sockets[pid];
            if (fd < 0) continue;

            client_packet_t pkt;
            int n = recv(fd, &pkt, sizeof(pkt), 0);
            if (n <= 0 || pkt.packet_type == LEAVE) {
                server_packet_t ack = { .packet_type = ACK };
                send(fd, &ack, sizeof(ack), 0);
                close(fd);
                game.sockets[pid] = -1;
                game.player_status[pid] = PLAYER_LEFT;
            } else if (pkt.packet_type == READY) {
                game.player_status[pid] = PLAYER_ACTIVE;
                ready_count++;
                server_packet_t ready_ack = { .packet_type = ACK };
                send(fd, &ready_ack, sizeof(ready_ack), 0);
            }
        }

        if (ready_count < 2) {
            for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
                int fd = game.sockets[pid];
                if (fd >= 0) {
                    server_packet_t halt = { .packet_type = HALT };
                    send(fd, &halt, sizeof(halt), 0);
                    close(fd);
                }
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
        game.current_player = next_active_player(&game, game.dealer_player);
        broadcast_info();

        for (int round = 0; round < 4; round++) {
            memset(game.current_bets, 0, sizeof(game.current_bets));
            game.highest_bet = 0;
            int actions = 0, num_active = count_active_players();

            while (actions < num_active) {
                client_packet_t in;
                recv(game.sockets[game.current_player], &in, sizeof(in), 0);
                server_packet_t resp;
                int ok = handle_client_action(&game, game.current_player, &in, &resp);
                send(game.sockets[game.current_player], &resp, sizeof(resp), 0);

                if (ok == 0 && resp.packet_type == ACK) {
                    actions++;
                    game.current_player = next_active_player(&game, game.current_player);
                    broadcast_info();
                }
            }
            if (round < 3) {
                server_community(&game);
                broadcast_info();
            }
        }

        int winner = find_winner(&game);
        game.player_stacks[winner] += game.pot_size;
        broadcast_end(winner);
    }

    printf("[Server] Shutting down.\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game.player_status[i] != PLAYER_LEFT)
            close(game.sockets[i]);
    }
    return 0;
}
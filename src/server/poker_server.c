#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"

extern player_id_t next_active_player(game_state_t *g, player_id_t start);

#define BASE_PORT   2201
#define NUM_PORTS   6
#define BACKLOG     4
#define MAX_PLAYERS 6

int has_acted[MAX_PLAYERS] = {0};
int last_raiser            = -1;

static int  server_fds[NUM_PORTS] = { -1 };

game_state_t game;

static int recv_full(int fd, void *buf, size_t len)
{
    size_t r = 0;
    while (r < len) {
        ssize_t n = recv(fd, (char*)buf + r, len - r, 0);
        if (n <= 0) return -1;
        r += (size_t)n;
    }
    return 0;
}

static int send_full(int fd, const void *buf, size_t len)
{
    size_t s = 0;
    while (s < len) {
        ssize_t n = send(fd, (const char*)buf + s, len - s, 0);
        if (n <= 0) return -1;
        s += (size_t)n;
    }
    return 0;
}

static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

static void reset_for_new_street(game_state_t *g)
{
    g->highest_bet = 0;
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        g->pot_size        += g->current_bets[p];
        g->current_bets[p]  = 0;
    }
    memset(has_acted, 0, sizeof has_acted);
}

static void clear_after_raise(void)
{
    memset(has_acted, 0, sizeof has_acted);
}

static void rotate_dealer(game_state_t *g)
{
    for (int i = 1; i <= MAX_PLAYERS; ++i) {
        player_id_t cand = (g->dealer_player + i) % MAX_PLAYERS;
        if (g->player_status[cand] != PLAYER_LEFT) {
            g->dealer_player = cand;
            return;
        }
    }
}

static int wait_for_ready(void)
{
    struct timeval TIMEOUT = { .tv_sec = 5, .tv_usec = 0 };
    int ready_cnt     = 0;
    int responded_cnt = 0;
    int responded[NUM_PORTS] = {0};

    while (responded_cnt < NUM_PORTS) {
        fd_set rfds; FD_ZERO(&rfds);
        int maxfd = -1;
        for (int s = 0; s < NUM_PORTS; ++s) {
            if (game.player_status[s] != PLAYER_LEFT && !responded[s]) {
                FD_SET(game.sockets[s], &rfds);
                if (game.sockets[s] > maxfd) maxfd = game.sockets[s];
            }
        }
        if (maxfd == -1) break;

        int sel = select(maxfd + 1, &rfds, NULL, NULL, &TIMEOUT);
        if (sel < 0) die("select");

        if (sel == 0) {
            for (int s = 0; s < NUM_PORTS; ++s) {
                if (!responded[s] && game.player_status[s] != PLAYER_LEFT) {
                    game.player_status[s] = PLAYER_LEFT;
                    close(game.sockets[s]);
                    responded[s] = 1;
                    ++responded_cnt;
                }
            }
            continue;
        }

        for (int s = 0; s < NUM_PORTS; ++s) {
            if (game.player_status[s] != PLAYER_LEFT && !responded[s] &&
                FD_ISSET(game.sockets[s], &rfds)) {
                client_packet_t pkt;
                if (recv_full(game.sockets[s], &pkt, sizeof pkt) == -1) {
                    game.player_status[s] = PLAYER_LEFT;
                    close(game.sockets[s]);
                } else if (pkt.packet_type == READY) {
                    ++ready_cnt;
                } else if (pkt.packet_type == LEAVE) {
                    game.player_status[s] = PLAYER_LEFT;
                    close(game.sockets[s]);
                }
                responded[s] = 1;
                ++responded_cnt;
            }
        }
    }
    return ready_cnt;
}

static void send_info_to_all(void)
{
    for (int s = 0; s < MAX_PLAYERS; ++s) {
        if (game.player_status[s] != PLAYER_LEFT) {
            server_packet_t pkt; build_info_packet(&game, s, &pkt);
            if (send_full(game.sockets[s], &pkt, sizeof pkt) == -1) {
                game.player_status[s] = PLAYER_LEFT;
                close(game.sockets[s]);
            }
        }
    }
}

static int count_active(void)
{
    int c = 0;
    for (int p = 0; p < MAX_PLAYERS; ++p)
        if (game.player_status[p] == PLAYER_ACTIVE || game.player_status[p] == PLAYER_ALLIN)
            ++c;
    return c;
}

int main(int argc, char **argv)
{
    for (int i = 0; i < NUM_PORTS; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) die("socket");
        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt) == -1)
            die("setsockopt");
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(BASE_PORT + i);
        if (bind(fd, (struct sockaddr*)&addr, sizeof addr) == -1) die("bind");
        if (listen(fd, BACKLOG) == -1) die("listen");
        server_fds[i] = fd;
    }

    int seed = (argc == 2) ? atoi(argv[1]) : 0;
    init_game_state(&game, 100, seed);

    for (int seat = 0; seat < NUM_PORTS; ++seat) {
        struct sockaddr_in caddr; socklen_t alen = sizeof caddr;
        int cfd = accept(server_fds[seat], (struct sockaddr*)&caddr, &alen);
        if (cfd == -1) die("accept");

        client_packet_t join;
        if (recv_full(cfd, &join, sizeof join) == -1 || join.packet_type != JOIN) {
            --seat; close(cfd); continue;
        }
        game.sockets[seat]       = cfd;
        game.player_status[seat] = PLAYER_ACTIVE;
        fprintf(stderr, "[Server] Player %d connected (port %d)\n", seat, BASE_PORT + seat);
    }

    while (1) {
        if (wait_for_ready() < 2) {
            server_packet_t halt = { .packet_type = HALT };
            for (int s = 0; s < NUM_PORTS; ++s)
                if (game.player_status[s] != PLAYER_LEFT)
                    send_full(game.sockets[s], &halt, sizeof halt);
            break;
        }

        reset_game_state(&game);
        server_deal(&game);
        memset(has_acted, 0, sizeof has_acted);
        last_raiser = -1;
        send_info_to_all();

        while (1) {
            while (!check_betting_end(&game)) {
                player_id_t pid = game.current_player;
                if (game.player_status[pid] != PLAYER_ACTIVE) {
                    game.current_player = next_active_player(&game, (pid + 1) % MAX_PLAYERS);
                    continue;
                }

                client_packet_t in;
                if (recv_full(game.sockets[pid], &in, sizeof in) == -1) {
                    game.player_status[pid] = PLAYER_LEFT;
                    close(game.sockets[pid]);
                    game.current_player = next_active_player(&game, (pid + 1) % MAX_PLAYERS);
                    continue;
                }

                server_packet_t rsp;
                if (handle_client_action(&game, pid, &in, &rsp) == 0 && rsp.packet_type == ACK) {
                    has_acted[pid] = 1;
                    if (in.packet_type == RAISE) {
                        last_raiser = pid;
                        clear_after_raise();
                        has_acted[pid] = 1;
                    }
                    game.current_player = next_active_player(&game, (pid + 1) % MAX_PLAYERS);
                }

                send_full(game.sockets[pid], &rsp, sizeof rsp);
                if (rsp.packet_type == ACK) send_info_to_all();
            }

            if (count_active() <= 1) {
                player_id_t winner = -1;
                for (int p = 0; p < MAX_PLAYERS; ++p)
                    if (game.player_status[p] == PLAYER_ACTIVE || game.player_status[p] == PLAYER_ALLIN) { winner = p; break; }
                if (winner != -1) { game.player_stacks[winner] += game.pot_size; game.pot_size = 0; }
                server_packet_t ep; build_end_packet(&game, winner, &ep);
                for (int s = 0; s < NUM_PORTS; ++s)
                    if (game.player_status[s] != PLAYER_LEFT)
                        send_full(game.sockets[s], &ep, sizeof ep);
                break;
            }

            if (game.round_stage == ROUND_RIVER) break;

            server_community(&game);
            reset_for_new_street(&game);
            last_raiser = -1;
            send_info_to_all();
        }

        if (game.round_stage == ROUND_RIVER) {
            player_id_t winner = find_winner(&game);
            if (winner == -1) {
                for (int p = 0; p < MAX_PLAYERS; ++p)
                    if (game.player_status[p] == PLAYER_ACTIVE || game.player_status[p] == PLAYER_ALLIN) { winner = p; break; }
            }
            if (winner != -1) { game.player_stacks[winner] += game.pot_size; game.pot_size = 0; }
            server_packet_t endpkt; build_end_packet(&game, winner, &endpkt);
            for (int s = 0; s < NUM_PORTS; ++s)
                if (game.player_status[s] != PLAYER_LEFT)
                    send_full(game.sockets[s], &endpkt, sizeof endpkt);
        }

        rotate_dealer(&game);
    }

    for (int i = 0; i < NUM_PORTS; ++i) {
        close(server_fds[i]);
        if (game.player_status[i] != PLAYER_LEFT)
            close(game.sockets[i]);
    }
    return 0;
}
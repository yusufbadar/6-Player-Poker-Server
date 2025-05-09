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

#define BASE_PORT 2201
#define NUM_PORTS 6
#define BUFFER_SIZE 1024
#define BACKLOG 4
#define MAX_PLAYERS 6

int has_acted[MAX_PLAYERS] = {0};
int last_raiser = -1;
typedef struct {
    int socket;
    struct sockaddr_in address;
} player_t;

static int server_fds[NUM_PORTS] = { -1 };

game_state_t game; //global variable to store our game state info (this is a huge hint for you)

static void kill(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}
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
int wait_for_ready(void) {
    int ready_cnt = 0;
    int responded_cnt = 0;
    int responded[NUM_PORTS] = {0};

    while (responded_cnt < NUM_PORTS) {
        fd_set rfds; 
        FD_ZERO(&rfds);
        int maxfd = -1;
        for (int s = 0; s < NUM_PORTS; ++s) {
            if (game.player_status[s] != PLAYER_LEFT && !responded[s]) {
                FD_SET(game.sockets[s], &rfds);
                if (game.sockets[s] > maxfd) maxfd = game.sockets[s];
            }
        }

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0)
            kill("select");

        for (int s = 0; s < NUM_PORTS; ++s) {
            if (!responded[s] && FD_ISSET(game.sockets[s], &rfds)) {
                client_packet_t pkt;
                if (recv_full(game.sockets[s], &pkt, sizeof(pkt)) == -1) {
                    game.player_status[s] = PLAYER_LEFT;
                    close(game.sockets[s]);
                } else if (pkt.packet_type == READY) {
                    ready_cnt++;
                } else if (pkt.packet_type == LEAVE) {
                    game.player_status[s] = PLAYER_LEFT;
                    close(game.sockets[s]);
                }
                responded[s] = 1;
                responded_cnt++;
            }
        }
    }

    return ready_cnt;
}
static inline player_id_t next_active_player(game_state_t *g, player_id_t start)
{
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        player_id_t p = (start + i) % MAX_PLAYERS;
        if (g->player_status[p] == PLAYER_ACTIVE) return p;
    }
    return -1;
}
int main(int argc, char **argv)
{
    for (int i = 0; i < NUM_PORTS; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) kill("socket");
        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
            kill("setsockopt");
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(BASE_PORT + i);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) kill("bind");
        if (listen(fd, BACKLOG) == -1) kill("listen");
        server_fds[i] = fd;
    }

    int seed = (argc == 2) ? atoi(argv[1]) : 0;
    init_game_state(&game, 100, seed);

    for (int seat = 0; seat < NUM_PORTS; ++seat) {
        struct sockaddr_in caddr; socklen_t alen = sizeof(caddr);
        int cfd = accept(server_fds[seat], (struct sockaddr *)&caddr, &alen);
        if (cfd == -1) kill("accept");

        client_packet_t pkt;
        if (recv_full(cfd, &pkt, sizeof(pkt)) == -1 || pkt.packet_type != JOIN) {
            --seat; close(cfd); continue;
        }
        game.sockets[seat] = cfd;
        game.player_status[seat] = PLAYER_ACTIVE;
        printf("[Server] Player %d connected.\n", seat);
    }

    while (1) {

        int num_ready = wait_for_ready();
        if (num_ready < 2) {
            server_packet_t halt = { .packet_type = HALT };
            for (int s = 0; s < NUM_PORTS; ++s) {
                if (game.player_status[s] != PLAYER_LEFT)
                    send(game.sockets[s], &halt, sizeof(halt), 0);
            }
            break;
        }

        reset_game_state(&game);
        server_deal(&game);
        memset(has_acted, 0, sizeof(has_acted));
        last_raiser = -1;
        for (int s = 0; s < NUM_PORTS; ++s) {
            if (game.player_status[s] == PLAYER_LEFT) continue;
            server_packet_t ip; build_info_packet(&game, s, &ip);
            send(game.sockets[s], &ip, sizeof(ip), 0);
        }

        while (1) {
            while (!check_betting_end(&game)) {
                player_id_t pid = game.current_player;
                if (game.player_status[pid] != PLAYER_ACTIVE) {
                    game.current_player = (pid + 1) % MAX_PLAYERS;
                    continue;
                }

                client_packet_t in;
                if (recv_full(game.sockets[pid], &in, sizeof(in)) == -1) {
                    game.player_status[pid] = PLAYER_LEFT;
                    close(game.sockets[pid]);
                    game.current_player = next_active_player(&game, (pid + 1) % MAX_PLAYERS);
                    continue;
                }

                server_packet_t acknack;
                handle_client_action(&game, pid, &in, &acknack);
                send(game.sockets[pid], &acknack, sizeof(acknack), 0);

                for (int s = 0; s < NUM_PORTS; ++s) {
                    if (game.player_status[s] == PLAYER_LEFT) continue;
                    server_packet_t info;
                    build_info_packet(&game, s, &info);
                    send(game.sockets[s], &info, sizeof(info), 0);
                }
            }

            int survivors = 0, surv = -1;
            for (int s = 0; s < NUM_PORTS; ++s)
                if (game.player_status[s] == PLAYER_ACTIVE) { ++survivors; surv = s; }

            if (survivors == 1) {
                game.player_stacks[surv] += game.pot_size;
                game.pot_size = 0;

                server_packet_t ep; build_end_packet(&game, surv, &ep);
                for (int s = 0; s < NUM_PORTS; ++s){
                    if (game.player_status[s] != PLAYER_LEFT){
                        send(game.sockets[s], &ep, sizeof(ep), 0);
                    }
                }
                continue;
            }

            if (game.round_stage == ROUND_RIVER) break;
            server_community(&game);
            memset(has_acted, 0, sizeof(has_acted));
            last_raiser = -1;
            for (int s = 0; s < NUM_PORTS; ++s) {
                if (game.player_status[s] == PLAYER_LEFT) continue;
                server_packet_t info; build_info_packet(&game, s, &info);
                send(game.sockets[s], &info, sizeof(info), 0);
            }
        }

        player_id_t winner = find_winner(&game);
        if (winner == -1) {
            for (int s = 0; s < MAX_PLAYERS; ++s)
                if (game.player_status[s] == PLAYER_ACTIVE) { winner = s; break; }
        }
        game.player_stacks[winner] += game.pot_size;

        server_packet_t end_pkt; build_end_packet(&game, winner, &end_pkt);
        for (int s = 0; s < NUM_PORTS; ++s){
            if (game.player_status[s] != PLAYER_LEFT){
                send(game.sockets[s], &end_pkt, sizeof(end_pkt), 0);
            }
        }
    }
    // Close all fds (you're welcome)
    for (int i = 0; i < MAX_PLAYERS; i++) {
        close(server_fds[i]);
        if (game.player_status[i] != PLAYER_LEFT) {
            close(game.sockets[i]);
        }
    }

    return 0;
}
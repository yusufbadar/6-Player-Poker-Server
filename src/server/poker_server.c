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

#define BASE_PORT   2201
#define NUM_PORTS   6
#define BUFFER_SIZE 1024

int has_acted[MAX_PLAYERS] = {0};
int last_raiser = -1;
game_state_t game;

static void shuffle_state(void) {
    shuffle_deck(game.deck);
    game.round_stage = ROUND_INIT;
    game.next_card = 0;
    game.highest_bet = 0;
    game.pot_size = 0;
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (game.player_status[p] != PLAYER_LEFT) game.player_status[p] = PLAYER_ACTIVE;
        game.current_bets[p] = 0;
        game.player_hands[p][0] = game.player_hands[p][1] = NOCARD;
    }
    for (int c = 0; c < MAX_COMMUNITY_CARDS; ++c) game.community_cards[c] = NOCARD;
}

void reset_game_state(game_state_t *g) {
    shuffle_state();
}

static player_id_t next_active_player(void) {
    int cur = game.current_player;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        cur = (cur + 1) % MAX_PLAYERS;
        if (game.player_status[cur] == PLAYER_ACTIVE) return cur;
    }
    return -1;
}

static void send_packet_all(server_packet_t *pkt) {
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int fd = game.sockets[pid];
        if (fd >= 0) send(fd, pkt, sizeof(*pkt), 0);
    }
}

static void broadcast_info(void) {
    server_packet_t pkt;
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        if (game.sockets[pid] < 0) continue;
        build_info_packet(&game, pid, &pkt);
        send(game.sockets[pid], &pkt, sizeof(pkt), 0);
    }
}

static void broadcast_end(int winner) {
    server_packet_t pkt;
    build_end_packet(&game, winner, &pkt);
    send_packet_all(&pkt);
}

static int count_active_players(void) {
    int n = 0;
    for (int p = 0; p < MAX_PLAYERS; ++p) if (game.player_status[p] == PLAYER_ACTIVE) ++n;
    return n;
}

static void wait_ready_or_leave(void) {
    while (1) {
        int ready_count = 0;
        for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
            int fd = game.sockets[pid];
            if (fd < 0) continue;
            client_packet_t in;
            int n = recv(fd, &in, sizeof(in), MSG_DONTWAIT);
            if (n <= 0) continue;
            if (in.packet_type == LEAVE) {
                server_packet_t ack = { .packet_type = ACK };
                send(fd, &ack, sizeof(ack), 0);
                close(fd);
                game.sockets[pid] = -1;
                game.player_status[pid] = PLAYER_LEFT;
                continue;
            }
            if (in.packet_type == READY) game.player_status[pid] = PLAYER_ACTIVE;
        }
        for (int p = 0; p < MAX_PLAYERS; ++p) if (game.player_status[p] == PLAYER_ACTIVE) ++ready_count;
        if (ready_count >= 2) return;
        if (ready_count < 2) {
            server_packet_t halt = { .packet_type = HALT };
            send_packet_all(&halt);
            exit(0);
        }
    }
}

static void rotate_dealer(void) {
    if (game.dealer_player < 0) {
        for (int i = 0; i < MAX_PLAYERS; ++i) if (game.player_status[i] == PLAYER_ACTIVE) { game.dealer_player = i; break; }
    } else {
        do { game.dealer_player = (game.dealer_player + 1) % MAX_PLAYERS; } while (game.player_status[game.dealer_player] != PLAYER_ACTIVE);
    }
}

static int play_street(void) {
    memset(has_acted, 0, sizeof has_acted);
    memset(game.current_bets, 0, sizeof game.current_bets);
    game.highest_bet = 0;
    game.current_player = next_active_player();
    int num_active = count_active_players();
    int actions = 0;
    while (actions < num_active) {
        client_packet_t in;
        recv(game.sockets[game.current_player], &in, sizeof(in), 0);
        server_packet_t resp;
        int ok = handle_client_action(&game, game.current_player, &in, &resp);
        send(game.sockets[game.current_player], &resp, sizeof(resp), 0);
        if (ok == 0) {
            if (in.packet_type == RAISE) { num_active = count_active_players(); actions = 1; }
            else ++actions;
            if (count_active_players() == 1) return 1;
            if (actions < num_active) {
                game.current_player = next_active_player();
                broadcast_info();
            }
        }
    }
    return 0;
}

static void deal_community_if_needed(int stage) {
    if (stage < 3) {
        server_community(&game);
        memset(game.current_bets, 0, sizeof game.current_bets);
        game.highest_bet = 0;
        game.current_player = (game.dealer_player + 1) % MAX_PLAYERS;
        while (game.player_status[game.current_player] != PLAYER_ACTIVE) game.current_player = next_active_player();
        broadcast_info();
    }
}

static void play_hand(void) {
    reset_game_state(&game);
    rotate_dealer();
    server_deal(&game);
    game.round_stage = ROUND_PREFLOP;
    game.current_player = (game.dealer_player + 1) % MAX_PLAYERS;
    while (game.player_status[game.current_player] != PLAYER_ACTIVE) game.current_player = (game.current_player + 1) % MAX_PLAYERS;
    broadcast_info();
    int short_circuit = 0;
    for (int stage = 0; stage < 4 && !short_circuit; ++stage) {
        short_circuit = play_street();
        deal_community_if_needed(stage);
    }
    int winner = find_winner(&game);
    game.player_stacks[winner] += game.pot_size;
    broadcast_end(winner);
}

int main(int argc, char **argv) {
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
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(BASE_PORT + i);
        assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(listen(fd, 1) == 0);
        server_fds[i] = fd;
    }
    int seed = (argc == 2 ? atoi(argv[1]) : 0);
    init_game_state(&game, 100, seed);
    for (int pid = 0; pid < MAX_PLAYERS; ++pid) {
        int cfd = accept(server_fds[pid], NULL, NULL);
        game.sockets[pid] = cfd;
        game.player_status[pid] = PLAYER_ACTIVE;
        game.num_players++;
        client_packet_t join_pkt;
        recv(cfd, &join_pkt, sizeof(join_pkt), 0);
        assert(join_pkt.packet_type == JOIN);
    }
    for (int i = 0; i < NUM_PORTS; ++i) close(server_fds[i]);
    game.dealer_player = -1;
    while (1) {
        wait_ready_or_leave();
        play_hand();
    }
    return 0;
}
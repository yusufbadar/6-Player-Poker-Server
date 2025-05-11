#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"
#include "logs.h"

// for debugging
void print_game_state(game_state_t *game) {
    (void) game;
}

void init_deck(card_t deck[DECK_SIZE], int seed) { // DO NOT TOUCH
    srand(seed);
    int i = 0;
    for (int r = 0; r < 13; ++r) {
        for (int s = 0; s < 4; ++s) {
            deck[i++] = (r << SUITE_BITS) | s;
        }
    }
}

void shuffle_deck(card_t deck[DECK_SIZE]) { // DO NOT TOUCH
    for (int i = 0; i < DECK_SIZE; ++i) {
        int j = rand() % DECK_SIZE;
        card_t temp = deck[i];
        deck[i] = deck[j];
        deck[j] = temp;
    }
}

void init_game_state(game_state_t *g, int starting_stack, int seed) {
    memset(g, 0, sizeof(*g));
    init_deck(g->deck, seed);

    g->round_stage = ROUND_INIT;
    g->dealer_player = -1;
    g->current_player = -1;

    for (int i = 0; i < MAX_PLAYERS; ++i) {
        g->player_stacks[i] = starting_stack;
        g->player_status[i] = PLAYER_LEFT;
        g->current_bets[i] = 0;
        g->player_hands[i][0] = g->player_hands[i][1] = NOCARD;
    }
    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i) {
        g->community_cards[i] = NOCARD;
    }

    g->next_card = 0;
    g->highest_bet = 0;
    g->pot_size = 0;
}

void server_join(game_state_t *g) {
    int pid = g->current_player;
    if (g->player_status[pid] == PLAYER_LEFT) {
        g->player_status[pid] = PLAYER_ACTIVE;
        g->num_players++;
        log_info("Player %d joined (total=%d)", pid, g->num_players);
    }
}

int server_ready(game_state_t *g) {
    int pid = g->current_player;
    static int ready_count = 0;
    static int ready_flags[MAX_PLAYERS] = {0};

    if (!ready_flags[pid]) {
        ready_flags[pid] = 1;
        ready_count++;
        log_info("Player %d is ready (%d/%d)", pid, ready_count, MAX_PLAYERS);
    }
    if (ready_count < MAX_PLAYERS) return 0;

    for (int i = 0; i < MAX_PLAYERS; ++i) ready_flags[i] = 0;
    ready_count = 0;

    g->round_stage = ROUND_PREFLOP;
    g->next_card = 0;
    g->highest_bet = 0;
    g->pot_size = 0;

    server_deal(g);
    g->current_player = (g->dealer_player + 1) % MAX_PLAYERS;

    return 1;
}

void server_deal(game_state_t *g) {
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (g->player_status[i] == PLAYER_ACTIVE) {
            g->player_hands[i][0] = g->deck[g->next_card++];
            g->player_hands[i][1] = g->deck[g->next_card++];
        }
    }
}

int check_betting_end(game_state_t *game) {
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (game->player_status[i] == PLAYER_ACTIVE &&
            game->current_bets[i] != game->highest_bet) {
            return 0;
        }
    }
    return 1;
}

int server_bet(game_state_t *g) {
    if (check_betting_end(g)) return 1;
    do {
        g->current_player = (g->current_player + 1) % MAX_PLAYERS;
    } while (g->player_status[g->current_player] != PLAYER_ACTIVE);
    return 0;
}

void server_community(game_state_t *g) {
    if (g->round_stage == ROUND_PREFLOP) {
        for (int i = 0; i < 3; ++i)
            g->community_cards[i] = g->deck[g->next_card++];
        g->round_stage = ROUND_FLOP;
    } else if (g->round_stage == ROUND_FLOP) {
        g->community_cards[3] = g->deck[g->next_card++];
        g->round_stage = ROUND_TURN;
    } else if (g->round_stage == ROUND_TURN) {
        g->community_cards[4] = g->deck[g->next_card++];
        g->round_stage = ROUND_RIVER;
    }
    g->current_player = (g->dealer_player + 1) % MAX_PLAYERS;
}

void server_end(game_state_t *g) {
    int winner = find_winner(g);
    log_info("Hand ended, winner = %d", winner);
    g->player_stacks[winner] += g->pot_size;
    g->round_stage = ROUND_SHOWDOWN;
}

static int cmp_int_desc(const void *a, const void *b) {
    int ai = *(const int*)a;
    int bi = *(const int*)b;
    return bi - ai;
}

static int cmp_desc(const void *a, const void *b) {
    return (*(int*)b) - (*(int*)a);
}

int evaluate_hand(game_state_t *g, player_id_t p) {
    card_t deck7[7];
    int n = 0;
    deck7[n++] = g->player_hands[p][0];
    deck7[n++] = g->player_hands[p][1];
    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i) {
        if (g->community_cards[i] != NOCARD)
            deck7[n++] = g->community_cards[i];
    }
    return hand_value(deck7, n);
}

int find_winner(game_state_t *game) {
    int best_pid = -1;
    int best_val = -1;
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (game->player_status[p] == PLAYER_ACTIVE ||
            game->player_status[p] == PLAYER_ALLIN) {
            int val = evaluate_hand(game, p);
            if (val > best_val) {
                best_val = val;
                best_pid = p;
            }
        }
    }
    return best_pid;
}

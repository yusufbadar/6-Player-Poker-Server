#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"
#include "logs.h"

#define RANK_MASK  0xF0
#define SUIT_MASK  0x0F

int has_acted[MAX_PLAYERS] = {0};
int last_raiser            = -1;

void print_game_state(game_state_t *game) { (void)game; }

void init_deck(card_t deck[DECK_SIZE], int seed)
{
    srand(seed);
    int i = 0;
    for (int s = 0; s < 4; ++s)
        for (int r = 0; r < 13; ++r)
            deck[i++] = (r << SUITE_BITS) | s;   /* 2♣ … A♠ packed as 4‑bit rank|suit */
}

void shuffle_deck(card_t deck[DECK_SIZE])
{
    for (int i = 0; i < DECK_SIZE - 1; ++i) {
        int j = i + rand() % (DECK_SIZE - i);    /* unbiased Fisher–Yates */
        card_t tmp = deck[i]; deck[i] = deck[j]; deck[j] = tmp;
    }
}
static player_id_t first_active_after(game_state_t *g, player_id_t start)
{
    for (int i = 1; i <= MAX_PLAYERS; ++i) {
        player_id_t p = (start + i) % MAX_PLAYERS;
        if (g->player_status[p] == PLAYER_ACTIVE) return p;
    }
    return -1;
}

player_id_t next_active_player(game_state_t *g, player_id_t start)
{
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        player_id_t p = (start + i) % MAX_PLAYERS;
        if (g->player_status[p] == PLAYER_ACTIVE) return p;
    }
    return -1;
}

void init_game_state(game_state_t *g, int stack, int seed)
{
    memset(g, 0, sizeof *g);
    init_deck(g->deck, seed);
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        g->player_stacks[p] = stack;
        g->player_status[p] = PLAYER_LEFT;
        g->sockets[p]       = -1;
        g->player_hands[p][0] = g->player_hands[p][1] = NOCARD;
    }
    memset(g->community_cards, NOCARD, sizeof g->community_cards);
    g->dealer_player  = 0;
    g->current_player = 1;
    g->next_card      = 0;
    g->round_stage    = ROUND_JOIN;
}

void reset_game_state(game_state_t *g)
{
    shuffle_deck(g->deck);
    g->next_card = 0;
    g->round_stage = ROUND_INIT;
    g->pot_size    = 0;
    g->highest_bet = 0;
    memset(g->community_cards, NOCARD, sizeof g->community_cards);
    memset(g->current_bets, 0, sizeof g->current_bets);
    memset(has_acted, 0, sizeof has_acted);
    last_raiser = -1;

    if (g->round_stage != ROUND_JOIN)  {
        for (int i = 1; i <= MAX_PLAYERS; ++i) {
            int cand = (g->dealer_player + i) % MAX_PLAYERS;
            if (g->player_status[cand] != PLAYER_LEFT) {
                g->dealer_player = cand;
                break;
            }
        }
    }      
    
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        g->player_hands[p][0] = g->player_hands[p][1] = NOCARD;
        if (g->player_status[p] != PLAYER_LEFT) g->player_status[p] = PLAYER_ACTIVE;
    }
    g->current_player = next_active_player(g, (g->dealer_player + 1) % MAX_PLAYERS);
}

void server_deal(game_state_t *g)
{
    for (player_id_t p = 0; p < MAX_PLAYERS; ++p)
        if (g->player_status[p] == PLAYER_ACTIVE)
            g->player_hands[p][0] = g->deck[g->next_card++];
    for (player_id_t p = 0; p < MAX_PLAYERS; ++p)
        if (g->player_status[p] == PLAYER_ACTIVE)
            g->player_hands[p][1] = g->deck[g->next_card++];
    g->round_stage = ROUND_PREFLOP;
    g->highest_bet = 0;
    memset(g->current_bets, 0, sizeof g->current_bets);
    memset(has_acted, 0, sizeof has_acted);
    last_raiser = -1;
    g->current_player = first_active_after(g, g->dealer_player);
    if (g->current_player == -1) g->current_player = next_active_player(g, 0);
}

int check_betting_end(game_state_t *g)
{
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (g->player_status[p] == PLAYER_ACTIVE ||
            g->player_status[p] == PLAYER_ALLIN) {
             if (!has_acted[p]) return 0;
             if (g->current_bets[p] < g->highest_bet && g->player_stacks[p] > 0) return 0;
         }
    }
    return 1;
}

void server_community(game_state_t *g)
{
    switch (g->round_stage) {
        case ROUND_PREFLOP: for (int i = 0; i < 3; ++i) g->community_cards[i] = g->deck[g->next_card++]; g->round_stage = ROUND_FLOP; break;
        case ROUND_FLOP:    g->community_cards[3] = g->deck[g->next_card++]; g->round_stage = ROUND_TURN; break;
        case ROUND_TURN:    g->community_cards[4] = g->deck[g->next_card++]; g->round_stage = ROUND_RIVER; break;
        default: break;
    }
    g->highest_bet = 0;
    memset(g->current_bets, 0, sizeof g->current_bets);
    memset(has_acted,      0, sizeof has_acted);
    last_raiser = -1;
    g->current_player = next_active_player(g, (g->dealer_player + 1) % MAX_PLAYERS);
}

static int rank7(card_t c[], int n)
{
    int r[13] = {0}, s[4] = {0};
    for (int i = 0; i < n; ++i) { r[(c[i] & RANK_MASK) >> 4]++; s[c[i] & SUIT_MASK]++; }
    int flush = -1; for (int i = 0; i < 4; ++i) if (s[i] >= 5) { flush = i; break; }
    int str = -1, cons = 0; for (int i = 0; i < 13; ++i) { if (r[i]) { if (++cons >= 5) str = i; } else cons = 0; }
    if (r[0] && r[1] && r[2] && r[3] && r[12]) str = 3;
    int pair=-1,pair2=-1,trips=-1,quads=-1;
    for (int i = 12; i >= 0; --i) {
        if (r[i] == 4) quads = i;
        else if (r[i] == 3) trips = i;
        else if (r[i] == 2) { if (pair == -1) pair = i; else if (pair2 == -1) pair2 = i; }
    }
    if (str!=-1 && flush!=-1) return 800+str;
    if (quads!=-1)            return 700+quads;
    if (trips!=-1 && pair!=-1)return 600+trips;
    if (flush!=-1)            { for (int i=12;i>=0;--i) if(r[i]) return 500+i; }
    if (str!=-1)              return 400+str;
    if (trips!=-1)            return 300+trips;
    if (pair!=-1 && pair2!=-1)return 200+pair*13+pair2;
    if (pair!=-1)             return 100+pair;
    for (int i=12;i>=0;--i) if(r[i]) return i;
    return 0;
}

int evaluate_hand(game_state_t *g, player_id_t p)
{
    card_t c[7]; int n = 0;
    if (g->player_hands[p][0] != NOCARD) c[n++] = g->player_hands[p][0];
    if (g->player_hands[p][1] != NOCARD) c[n++] = g->player_hands[p][1];
    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i) if (g->community_cards[i] != NOCARD) c[n++] = g->community_cards[i];
    return rank7(c, n);
}

int find_winner(game_state_t *g)
{
    int best = -1, win = -1;
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (g->player_status[p] == PLAYER_ACTIVE || g->player_status[p] == PLAYER_ALLIN) {
            int sc = evaluate_hand(g, p);
            if (sc > best) { best = sc; win = p; }
        }
    }
    return win;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"
#include "logs.h"

int has_acted[MAX_PLAYERS] = {0};
int last_raiser = -1;

void print_game_state(game_state_t *g) { (void)g; }

void init_deck(card_t deck[DECK_SIZE], int seed)
{
    srand(seed);
    int idx = 0;
    for (int r = 0; r < 13; ++r)
        for (int s = 0; s < 4; ++s)
            deck[idx++] = (r << SUITE_BITS) | s;
}

void shuffle_deck(card_t deck[DECK_SIZE])
{
    for (int i = 0; i < DECK_SIZE; ++i) {
        int j = rand() % DECK_SIZE;
        card_t tmp = deck[i];
        deck[i] = deck[j];
        deck[j] = tmp;
    }
}

player_id_t next_active_player(const game_state_t *g, player_id_t start)
{
    for (int i = 1; i <= MAX_PLAYERS; ++i) {
        player_id_t p = (start + i) % MAX_PLAYERS;
        if (g->player_status[p] == PLAYER_ACTIVE)
            return p;
    }
    return (player_id_t)-1;
}

static player_id_t first_active_after(const game_state_t *g, player_id_t start)
{
    for (int i = 1; i <= MAX_PLAYERS; ++i) {
        player_id_t p = (start + i) % MAX_PLAYERS;
        if (g->player_status[p] == PLAYER_ACTIVE)
            return p;
    }
    return (player_id_t)-1;
}

void init_game_state(game_state_t *g, int starting_stack, int seed)
{
    memset(g, 0, sizeof *g);
    init_deck(g->deck, seed);
    g->round_stage = ROUND_INIT;
    g->dealer_player = -1;
    g->current_player = -1;
    g->next_card = 0;
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        g->player_stacks[p] = starting_stack;
        g->player_status[p] = PLAYER_LEFT;
        g->sockets[p] = -1;
        g->player_hands[p][0] = g->player_hands[p][1] = NOCARD;
    }
    memset(g->community_cards, NOCARD, sizeof g->community_cards);
}

void reset_game_state(game_state_t *g)
{
    shuffle_deck(g->deck);
    g->next_card = 0;
    g->round_stage = ROUND_INIT;
    g->pot_size = 0;
    g->highest_bet = 0;
    memset(g->community_cards, NOCARD, sizeof g->community_cards);
    memset(g->current_bets, 0, sizeof g->current_bets);
    memset(has_acted, 0, sizeof has_acted);
    last_raiser = -1;
    for (int p = 0; p < MAX_PLAYERS; ++p)
        if (g->player_status[p] != PLAYER_LEFT)
            g->player_status[p] = PLAYER_ACTIVE;
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
}

int check_betting_end(game_state_t *g)
{
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (g->player_status[p] == PLAYER_ACTIVE || g->player_status[p] == PLAYER_ALLIN) {
            if (!has_acted[p])
                return 0;
            if (g->current_bets[p] < g->highest_bet && g->player_stacks[p] > 0)
                return 0;
        }
    }
    return 1;
}

int server_bet(game_state_t *g)
{
    if (check_betting_end(g))
        return 1;
    g->current_player = first_active_after(g, g->current_player);
    return 0;
}

void server_community(game_state_t *g)
{
    switch (g->round_stage) {
        case ROUND_PREFLOP:
            for (int i = 0; i < 3; ++i)
                g->community_cards[i] = g->deck[g->next_card++];
            g->round_stage = ROUND_FLOP;
            break;
        case ROUND_FLOP:
            g->community_cards[3] = g->deck[g->next_card++];
            g->round_stage = ROUND_TURN;
            break;
        case ROUND_TURN:
            g->community_cards[4] = g->deck[g->next_card++];
            g->round_stage = ROUND_RIVER;
            break;
        default:
            break;
    }
    g->highest_bet = 0;
    memset(g->current_bets, 0, sizeof g->current_bets);
    memset(has_acted, 0, sizeof has_acted);
    last_raiser = -1;
    g->current_player = first_active_after(g, g->dealer_player);
}

void server_end(game_state_t *g)
{
    int winner = find_winner(g);
    log_info("Hand ended, winner = %d", winner);
    g->player_stacks[winner] += g->pot_size;
    g->round_stage = ROUND_SHOWDOWN;
}

#define CARD_RANK(c) (((c) & 0xF0) >> 4)
#define CARD_SUIT(c)  ((c) & 0x0F)

static int cmp_desc(const void *a, const void *b)
{
    return (*(int *)b) - (*(int *)a);
}

static int pack_detail(const int *vals, int n)
{
    int out = 0;
    for (int i = 0; i < n; ++i)
        out = (out << 4) | vals[i];
    return out;
}

static int hand_value(card_t *cards, int count)
{
    int rank_cnt[13] = {0}, suit_cnt[4] = {0};
    for (int i = 0; i < count; ++i) {
        int r = CARD_RANK(cards[i]);
        int s = CARD_SUIT(cards[i]);
        rank_cnt[r]++;
        suit_cnt[s]++;
    }

    int uniq[14], u = 0;
    for (int r = 12; r >= 0; --r)
        if (rank_cnt[r])
            uniq[u++] = r;
    if (rank_cnt[12])
        uniq[u++] = -1;

    int best_sf = -1;
    for (int s = 0; s < 4; ++s) if (suit_cnt[s] >= 5) {
        int sr[7], m = 0;
        for (int i = 0; i < count; ++i)
            if (CARD_SUIT(cards[i]) == s)
                sr[m++] = CARD_RANK(cards[i]);
        qsort(sr, m, sizeof *sr, cmp_desc);
        int uniq_sr[8], k = 0, last = -2;
        for (int i = 0; i < m; ++i)
            if (sr[i] != last) {
                uniq_sr[k++] = sr[i];
                last = sr[i];
            }
        if (last == 12)
            uniq_sr[k++] = -1;
        for (int i = 0; i + 4 < k; ++i)
            if (uniq_sr[i] - uniq_sr[i + 4] == 4) {
                best_sf = uniq_sr[i] < 0 ? 3 : uniq_sr[i];
                break;
            }
        if (best_sf >= 0)
            break;
    }

    if (best_sf >= 0)
        return (8 << 20) | (best_sf << 16);

    int four = -1;
    for (int r = 12; r >= 0; --r)
        if (rank_cnt[r] == 4) {
            four = r;
            break;
        }
    if (four >= 0) {
        int kc = -1;
        for (int r = 12; r >= 0; --r)
            if (r != four && rank_cnt[r]) {
                kc = r;
                break;
            }
        return (7 << 20) | (four << 16) | (kc << 12);
    }

    int three = -1, pair = -1;
    for (int r = 12; r >= 0; --r)
        if (rank_cnt[r] >= 3) {
            three = r;
            break;
        }
    if (three >= 0)
        for (int r = 12; r >= 0; --r)
            if (r != three && rank_cnt[r] >= 2) {
                pair = r;
                break;
            }
    if (three >= 0 && pair >= 0)
        return (6 << 20) | (three << 16) | (pair << 12);

    for (int s = 0; s < 4; ++s)
        if (suit_cnt[s] >= 5) {
            int vals[5], k = 0;
            for (int r = 12; r >= 0 && k < 5; --r)
                for (int i = 0; i < count && k < 5; ++i)
                    if (CARD_SUIT(cards[i]) == s && CARD_RANK(cards[i]) == r)
                        vals[k++] = r;
            return (5 << 20) | pack_detail(vals, 5);
        }

    int top_st = -1;
    for (int i = 0; i + 4 < u; ++i) {
        int hi = uniq[i] < 0 ? 3 : uniq[i];
        int lo = uniq[i + 4] < 0 ? 0 : uniq[i + 4];
        if (hi - lo == 4) {
            top_st = hi;
            break;
        }
    }
    if (top_st >= 0)
        return (4 << 20) | (top_st << 16);

    if (three >= 0) {
        int kicks[2], k = 0;
        for (int r = 12; r >= 0 && k < 2; --r)
            if (r != three && rank_cnt[r])
                kicks[k++] = r;
        return (3 << 20) | (three << 16) | pack_detail(kicks, 2);
    }

    int p1 = -1, p2 = -1;
    for (int r = 12; r >= 0; --r)
        if (rank_cnt[r] >= 2) {
            if (p1 < 0)
                p1 = r;
            else if (p2 < 0)
                p2 = r;
            if (p1 >= 0 && p2 >= 0)
                break;
        }
    if (p2 >= 0) {
        int kc = -1;
        for (int r = 12; r >= 0; --r)
            if (r != p1 && r != p2 && rank_cnt[r]) {
                kc = r;
                break;
            }
        int vals[3] = {p1, p2, kc};
        return (2 << 20) | pack_detail(vals, 3);
    }

    if (p1 >= 0) {
        int kicks[3], k = 0;
        for (int r = 12; r >= 0 && k < 3; --r)
            if (r != p1 && rank_cnt[r])
                kicks[k++] = r;
        int vals[4] = {p1, kicks[0], kicks[1], kicks[2]};
        return (1 << 20) | pack_detail(vals, 4);
    }

    int hc[5], h = 0;
    for (int r = 12; r >= 0 && h < 5; --r)
        if (rank_cnt[r])
            hc[h++] = r;
    while (h < 5)
        hc[h++] = 0;
    return pack_detail(hc, 5);
}

int evaluate_hand(game_state_t *g, player_id_t pid)
{
    card_t tmp[7]; int n = 0;
    if (g->player_hands[pid][0] != NOCARD) tmp[n++] = g->player_hands[pid][0];
    if (g->player_hands[pid][1] != NOCARD) tmp[n++] = g->player_hands[pid][1];
    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i)
        if (g->community_cards[i] != NOCARD) tmp[n++] = g->community_cards[i];
    return hand_value(tmp, n);
}

int find_winner(game_state_t *g)
{
    int best_pid = -1, best_val = -1;
    for (int p = 0; p < MAX_PLAYERS; ++p)
        if (g->player_status[p] == PLAYER_ACTIVE || g->player_status[p] == PLAYER_ALLIN) {
            int v = evaluate_hand(g, p);
            if (v > best_val) { best_val = v; best_pid = p; }
        }
    return best_pid;
}

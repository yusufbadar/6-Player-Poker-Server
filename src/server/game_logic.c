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

static int compare_int_desc(const void *lhs, const void *rhs)
{
    const int a = *(const int *)lhs;
    const int b = *(const int *)rhs;
    return b - a;
}

static int compare_rank_desc(const void *lhs, const void *rhs)
{
    return (*(const int *)rhs) - (*(const int *)lhs);
}

static int pack_detail(const int *vals, int n)
{
    int out = 0;
    for (int i = 0; i < n; ++i)
    {
        out = (out << 4) | vals[i];
    }
    return out;
}

void print_game_state(game_state_t *game) { (void)game; }

void init_deck(card_t deck[DECK_SIZE], int seed)
{
    srand(seed);
    int idx = 0;
    for (int r = 0; r < 13; ++r)
    {
        for (int s = 0; s < 4; ++s)
        {
            deck[idx++] = (r << SUITE_BITS) | s;
        }
    }
}

void shuffle_deck(card_t deck[DECK_SIZE])
{
    for (int i = 0; i < DECK_SIZE; ++i)
    {
        int j  = rand() % DECK_SIZE;
        card_t tmp = deck[i];
        deck[i] = deck[j];
        deck[j] = tmp;
    }
}

void init_game_state(game_state_t *st, int initial_stack, int seed)
{
    memset(st, 0, sizeof *st);
    init_deck(st->deck, seed);
    st->round_stage    = ROUND_INIT;
    st->dealer_player  = -1;
    st->current_player = -1;

    for (int seat = 0; seat < MAX_PLAYERS; ++seat)
    {
        st->player_stacks [seat]  = initial_stack;
        st->player_status [seat]  = PLAYER_LEFT;
        st->current_bets   [seat] = 0;
        st->player_hands   [seat][0] = NOCARD;
        st->player_hands   [seat][1] = NOCARD;
    }

    for (int c = 0; c < MAX_COMMUNITY_CARDS; ++c)
    {
        st->community_cards[c] = NOCARD;
    }

    st->next_card   = 0;
    st->highest_bet = 0;
    st->pot_size    = 0;
}

void server_join(game_state_t *g)
{
    player_id_t seat = g->current_player;

    if (g->player_status[seat] == PLAYER_LEFT)
    {
        g->player_status[seat] = PLAYER_ACTIVE;
        g->num_players        += 1;
        log_info("Player %d joined (total=%d)", seat, g->num_players);
    }
}

int server_ready(game_state_t *g)
{
    player_id_t seat = g->current_player;

    static int ready_total = 0;
    static int ready_seen[MAX_PLAYERS] = {0};

    if (!ready_seen[seat])
    {
        ready_seen[seat] = 1;
        ++ready_total;
        log_info("Player %d is ready (%d/%d)", seat, ready_total, MAX_PLAYERS);
    }

    if (ready_total < MAX_PLAYERS)
    {
        return 0;
    }

    memset(ready_seen, 0, sizeof ready_seen);
    ready_total = 0;

    g->round_stage = ROUND_PREFLOP;
    g->next_card   = 0;
    g->highest_bet = 0;
    g->pot_size    = 0;

    server_deal(g);
    g->current_player = (g->dealer_player + 1) % MAX_PLAYERS;
    return 1;
}

void server_deal(game_state_t *g)
{
    for (int seat = 0; seat < MAX_PLAYERS; ++seat)
    {
        if (g->player_status[seat] == PLAYER_ACTIVE)
        {
            g->player_hands[seat][0] = g->deck[g->next_card++];
            g->player_hands[seat][1] = g->deck[g->next_card++];
        }
    }
}

int check_betting_end(game_state_t *g)
{
    for (int seat = 0; seat < MAX_PLAYERS; ++seat)
    {
        if (g->player_status[seat] == PLAYER_ACTIVE &&
            g->current_bets[seat] != g->highest_bet)
        {
            return 0;
        }
    }
    return 1;
}

int server_bet(game_state_t *g)
{
    if (check_betting_end(g)) { return 1; }

    do
    {
        g->current_player = (g->current_player + 1) % MAX_PLAYERS;
    }
    while (g->player_status[g->current_player] != PLAYER_ACTIVE);

    return 0;
}

void server_community(game_state_t *g)
{
    switch (g->round_stage)
    {
        case ROUND_PREFLOP:
            for (int c = 0; c < 3; ++c)
            {
                g->community_cards[c] = g->deck[g->next_card++];
            }
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

    g->current_player = (g->dealer_player + 1) % MAX_PLAYERS;
}

void server_end(game_state_t *g)
{
    player_id_t champ = find_winner(g);
    log_info("Hand ended, winner = %d", champ);
    g->player_stacks[champ] += g->pot_size;
    g->round_stage = ROUND_SHOWDOWN;
}

static int hand_value(const card_t *cards, int count)
{
    int rank_cnt[13] = {0};
    int suit_cnt[4]  = {0};

    for (int i = 0; i < count; ++i)
    {
        int rank = cards[i] >> SUITE_BITS;
        int suit = cards[i] & ((1 << SUITE_BITS) - 1);
        rank_cnt[rank]++;   
        suit_cnt[suit]++;
    }

    int uniq[14], uniq_cnt = 0;
    for (int r = 12; r >= 0; --r)
    {
        if (rank_cnt[r]) { uniq[uniq_cnt++] = r; }
    }
    if (rank_cnt[12]) { uniq[uniq_cnt++] = -1; }

    int best_sf = -1;
    for (int s = 0; s < 4; ++s)
    {
        if (suit_cnt[s] < 5) { continue; }

        int sr[7], m = 0;
        for (int i = 0; i < count; ++i)
        {
            if ((cards[i] & ((1 << SUITE_BITS) - 1)) == s)
            {
                sr[m++] = cards[i] >> SUITE_BITS;
            }
        }

        qsort(sr, m, sizeof *sr, compare_rank_desc);

        int su[8], k = 0, last = -2;
        for (int i = 0; i < m; ++i)
        {
            if (sr[i] != last)
            {
                su[k++] = sr[i];
                last    = sr[i];
            }
        }
        if (last == 12) { su[k++] = -1; }

        for (int i = 0; i + 4 < k; ++i)
        {
            if (su[i] - su[i + 4] == 4)
            {
                best_sf = su[i];
                break;
            }
        }
        if (best_sf >= 0) { break; }
    }
    if (best_sf >= 0) { return (8 << 20) | (best_sf << 16); }

    for (int r = 12; r >= 0; --r)
    {
        if (rank_cnt[r] == 4)
        {
            int kicker = -1;
            for (int t = 12; t >= 0; --t)
            {
                if (t != r && rank_cnt[t]) { kicker = t; break; }
            }
            return (7 << 20) | (r << 16) | (kicker << 12);
        }
    }

    int three = -1, pair = -1;
    for (int r = 12; r >= 0; --r)
    {
        if (rank_cnt[r] >= 3) { three = r; break; }
    }
    if (three >= 0)
    {
        for (int r = 12; r >= 0; --r)
        {
            if (r != three && rank_cnt[r] >= 2) { pair = r; break; }
        }
    }
    if (three >= 0 && pair >= 0)
    {
        return (6 << 20) | (three << 16) | (pair << 12);
    }

    for (int s = 0; s < 4; ++s)
    {
        if (suit_cnt[s] >= 5)
        {
            int vals[5], k = 0;
            for (int r = 12; r >= 0 && k < 5; --r)
            {
                for (int i = 0; i < count && k < 5; ++i)
                {
                    if ((cards[i] & ((1 << SUITE_BITS) - 1)) == s &&
                        (cards[i] >> SUITE_BITS) == r)
                    {
                        vals[k++] = r;
                    }
                }
            }
            return (5 << 20) | pack_detail(vals, 5);
        }
    }

    int top_st = -1;
    for (int i = 0; i + 4 < uniq_cnt; ++i)
    {
        int hi = uniq[i] < 0 ? 3 : uniq[i];
        int lo = uniq[i + 4] < 0 ? 0 : uniq[i + 4];
        if (hi - lo == 4) { top_st = hi; break; }
    }
    if (top_st >= 0) { return (4 << 20) | (top_st << 16); }

    if (three >= 0)
    {
        int kick[2], k = 0;
        for (int r = 12; r >= 0 && k < 2; --r)
        {
            if (r != three && rank_cnt[r]) { kick[k++] = r; }
        }
        return (3 << 20) | (three << 16) | pack_detail(kick, 2);
    }

    int p1 = -1, p2 = -1;
    for (int r = 12; r >= 0; --r)
    {
        if (rank_cnt[r] >= 2)
        {
            if (p1 < 0) { p1 = r; }
            else if (p2 < 0) { p2 = r; break; }
        }
    }
    if (p1 >= 0 && p2 >= 0)
    {
        int kicker = -1;
        for (int r = 12; r >= 0; --r)
        {
            if (r != p1 && r != p2 && rank_cnt[r]) { kicker = r; break; }
        }
        int tmp[3] = {p1, p2, kicker};
        return (2 << 20) | pack_detail(tmp, 3);
    }

    if (p1 >= 0)
    {
        int kick[3], k = 0;
        for (int r = 12; r >= 0 && k < 3; --r)
        {
            if (r != p1 && rank_cnt[r]) { kick[k++] = r; }
        }
        int tmp[4] = {p1, kick[0], kick[1], kick[2]};
        return (1 << 20) | pack_detail(tmp, 4);
    }

    int hc[5], h = 0;
    for (int r = 12; r >= 0 && h < 5; --r)
    {
        if (rank_cnt[r]) { hc[h++] = r; }
    }
    while (h < 5) { hc[h++] = 0; }

    return (0 << 20) | pack_detail(hc, 5);
}

int evaluate_hand(game_state_t *g, player_id_t p)
{
    card_t buf[7];
    int    n = 0;

    if (g->player_hands[p][0] != NOCARD) { buf[n++] = g->player_hands[p][0]; }
    if (g->player_hands[p][1] != NOCARD) { buf[n++] = g->player_hands[p][1]; }

    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i)
    {
        if (g->community_cards[i] != NOCARD) { buf[n++] = g->community_cards[i]; }
    }
    return hand_value(buf, n);
}

int find_winner(game_state_t *g)
{
    int champion = -1;
    int best_val = -1;

    for (int seat = 0; seat < MAX_PLAYERS; ++seat)
    {
        if (g->player_status[seat] == PLAYER_ACTIVE ||
            g->player_status[seat] == PLAYER_ALLIN)
        {
            int val = evaluate_hand(g, seat);
            if (val > best_val)
            {
                best_val = val;
                champion = seat;
            }
        }
    }
    return champion;
}
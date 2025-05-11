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
void print_game_state(game_state_t *g) { (void) g; }

void init_deck(card_t deck[DECK_SIZE], int seed) {
    srand(seed);
    int i = 0;
    for (int r = 0; r < 13; ++r)
        for (int s = 0; s < 4; ++s)
            deck[i++] = (r << SUITE_BITS) | s;
}

void shuffle_deck(card_t deck[DECK_SIZE]) {
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
    g->round_stage    = ROUND_INIT;
    g->dealer_player  = -1;
    g->current_player = -1;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        g->player_stacks[i]   = starting_stack;
        g->player_status[i]   = PLAYER_LEFT;
        g->current_bets[i]    = 0;
        g->player_hands[i][0] = g->player_hands[i][1] = NOCARD;
    }
    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i)
        g->community_cards[i] = NOCARD;
    g->next_card   = 0;
    g->highest_bet = 0;
    g->pot_size    = 0;
}

// Handle READY notifications and start a new hand when all players ready
int server_ready(game_state_t *g) {
    int pid = g->current_player;
    static int ready_count = 0, ready_flags[MAX_PLAYERS] = {0};
    if (!ready_flags[pid]) {
        ready_flags[pid] = 1;
        ready_count++;
        log_info("Player %d is ready (%d/%d)", pid, ready_count, MAX_PLAYERS);
    }
    if (ready_count < MAX_PLAYERS)
        return 0;
    // reset ready flags for next hand
    for (int i = 0; i < MAX_PLAYERS; ++i)
        ready_flags[i] = 0;
    ready_count = 0;
    // rotate dealer button
    if (g->dealer_player < 0) {
        g->dealer_player = 0;
    } else {
        do {
            g->dealer_player = (g->dealer_player + 1) % MAX_PLAYERS;
        } while (g->player_status[g->dealer_player] != PLAYER_ACTIVE);
    }
    // initialize betting round
    g->round_stage   = ROUND_PREFLOP;
    g->next_card     = 0;
    g->highest_bet   = 0;
    g->pot_size      = 0;
    // deal hole cards
    server_deal(g);
    // first to act is left of dealer
    g->current_player = (g->dealer_player + 1) % MAX_PLAYERS;
    while (g->player_status[g->current_player] != PLAYER_ACTIVE)
        g->current_player = (g->current_player + 1) % MAX_PLAYERS;
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

int check_betting_end(game_state_t *g) {
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (g->player_status[i] == PLAYER_ACTIVE &&
            g->current_bets[i] != g->highest_bet)
            return 0;
    }
    return 1;
}

int server_bet(game_state_t *g) {
    if (check_betting_end(g))
        return 1;
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
        g->round_stage        = ROUND_TURN;
    } else if (g->round_stage == ROUND_TURN) {
        g->community_cards[4] = g->deck[g->next_card++];
        g->round_stage        = ROUND_RIVER;
    }
    // next to act
    g->current_player = (g->dealer_player + 1) % MAX_PLAYERS;
}

void server_end(game_state_t *g) {
    int winner = find_winner(g);
    log_info("Hand ended, winner = %d", winner);
    g->player_stacks[winner] += g->pot_size;
    g->round_stage = ROUND_SHOWDOWN;
}

static int cmp_desc(const void *a, const void *b) {
    return (*(int *)b) - (*(int *)a);
}

static int pack_nibbles(const int *vals, int n) {
    int out = 0;
    for (int i = 0; i < n; ++i)
        out = (out << 4) | vals[i];
    return out;
}

static int hand_value(const card_t *cards, int n) {
    int rank_cnt[13] = {0}, suit_cnt[4] = {0};
    int ranks[7], suits[7];
    for (int i = 0; i < n; ++i) {
        suits[i] = cards[i] & ((1 << SUITE_BITS) - 1);
        ranks[i] = cards[i] >> SUITE_BITS;
        rank_cnt[ranks[i]]++;
        suit_cnt[suits[i]]++;
    }
    int uniq[14], u = 0;
    for (int r = 12; r >= 0; --r)
        if (rank_cnt[r]) uniq[u++] = r;
    if (rank_cnt[12]) uniq[u++] = -1;
    // straight flush
    for (int s = 0; s < 4; ++s) {
        if (suit_cnt[s] >= 5) {
            int sr[7], m = 0, last = -2, sf = -1;
            for (int i = 0; i < n; ++i)
                if (suits[i] == s) sr[m++] = ranks[i];
            qsort(sr, m, sizeof *sr, cmp_desc);
            int u2[7], k = 0;
            for (int i = 0; i < m; ++i) if (sr[i] != last) { u2[k++] = sr[i]; last = sr[i]; }
            if (last == 12) u2[k++] = -1;
            for (int i = 0; i + 4 < k; ++i) if (u2[i] - u2[i+4] == 4) { sf = u2[i]<0?3:u2[i]; break; }
            if (sf >= 0) return (8<<20)|(sf<<16);
        }
    }
    // four of a kind
    for (int r = 12; r>=0; --r) if (rank_cnt[r]==4) {
        int kicker=-1; for (int k=12;k>=0;--k) if (k!=r && rank_cnt[k]) { kicker=k; break; }
        return (7<<20)|(r<<16)|(kicker<<12);
    }
    // full house
    int three=-1, pair=-1;
    for (int r=12;r>=0;--r) if (rank_cnt[r]>=3) { three=r; break; }
    if (three>=0) for (int r=12;r>=0;--r) if (r!=three && rank_cnt[r]>=2) { pair=r; break; }
    if (three>=0 && pair>=0) return (6<<20)|(three<<16)|(pair<<12);
    // flush
    for (int s=0;s<4;++s) if (suit_cnt[s]>=5) {
        int vals[5], k=0;
        for (int r=12;r>=0&&k<5;--r) for (int i=0;i<n&&k<5;++i) if (suits[i]==s && ranks[i]==r) vals[k++]=r;
        return (5<<20)|pack_nibbles(vals,5);
    }
    // straight
    for (int i=0;i+4<u;++i) {
        int hi=uniq[i]<0?3:uniq[i], lo=uniq[i+4]<0?0:uniq[i+4];
        if (hi-lo==4) return (4<<20)|(hi<<16);
    }
    // three of a kind
    if (three>=0) {
        int kick[2], k=0;
        for (int r=12;r>=0&&k<2;--r) if (r!=three && rank_cnt[r]) kick[k++]=r;
        return (3<<20)|(three<<16)|pack_nibbles(kick,2);
    }
    // two pair
    int p1=-1,p2=-1;
    for (int r=12;r>=0;--r) if (rank_cnt[r]>=2) { if (p1<0) p1=r; else if (p2<0) { p2=r; break; }}
    if (p2>=0) {
        int kc=-1; for (int r=12;r>=0;--r) if (r!=p1&&r!=p2&&rank_cnt[r]) { kc=r; break; }
        int tmp[3]={p1,p2,kc}; return (2<<20)|pack_nibbles(tmp,3);
    }
    // one pair
    if (p1>=0) {
        int kick[3], k=0;
        for (int r=12;r>=0&&k<3;--r) if (r!=p1 && rank_cnt[r]) kick[k++]=r;
        int tmp[4]={p1,kick[0],kick[1],kick[2]}; return (1<<20)|pack_nibbles(tmp,4);
    }
    // high card
    int hc[5], h=0;
    for (int r=12;r>=0&&h<5;--r) if (rank_cnt[r]) hc[h++]=r;
    while (h<5) hc[h++]=0;
    return (0<<20)|pack_nibbles(hc,5);
}

int evaluate_hand(game_state_t *g, player_id_t p) {
    card_t cards[7]; int n=0;
    if (g->player_hands[p][0]!=NOCARD) cards[n++]=g->player_hands[p][0];
    if (g->player_hands[p][1]!=NOCARD) cards[n++]=g->player_hands[p][1];
    for (int i=0;i<MAX_COMMUNITY_CARDS;++i) if (g->community_cards[i]!=NOCARD) cards[n++]=g->community_cards[i];
    return hand_value(cards,n);
}

int find_winner(game_state_t *g) {
    int best=-1, val=-1;
    for (int i=0;i<MAX_PLAYERS;++i) if (g->player_status[i]==PLAYER_ACTIVE || g->player_status[i]==PLAYER_ALLIN) {
        int v=evaluate_hand(g,i);
        if (v>val) { val=v; best=i; }
    }
    return best;
}

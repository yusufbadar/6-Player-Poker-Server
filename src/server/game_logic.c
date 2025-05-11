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

void print_game_state(game_state_t *game) { (void)game; }

void init_deck(card_t deck[DECK_SIZE], int seed){
    srand(seed);
    int i = 0;
    for(int r = 0; r<13; r++){
        for(int s = 0; s<4; s++){
            deck[i++] = (r << SUITE_BITS) | s;
        }
    }
}

void shuffle_deck(card_t deck[DECK_SIZE]){
    for(int i = 0; i<DECK_SIZE; i++){
        int j = rand() % DECK_SIZE;
        card_t temp = deck[i];
        deck[i] = deck[j];
        deck[j] = temp;
    }
}

player_id_t next_active_player(const game_state_t *g, player_id_t start){
    for (int i = 1; i <= MAX_PLAYERS; ++i) {
        player_id_t p = (start + i) % MAX_PLAYERS;
        if (g->player_status[p] == PLAYER_ACTIVE)
            return p;
    }
    return (player_id_t)-1;
}

void init_game_state(game_state_t *g, int starting_stack, int seed){
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
    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i)
        g->community_cards[i] = NOCARD;

    g->next_card = 0;
    g->highest_bet = 0;
    g->pot_size = 0;
}

void reset_game_state(game_state_t *g){
    shuffle_deck(g->deck);
    g->round_stage = ROUND_INIT;
    g->next_card = 0;
    g->highest_bet = 0;
    g->pot_size = 0;

    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (g->player_status[i] != PLAYER_LEFT)
            g->player_status[i] = PLAYER_ACTIVE;
        g->current_bets[i] = 0;
        g->player_hands[i][0] = g->player_hands[i][1] = NOCARD;
    }
    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i)
        g->community_cards[i] = NOCARD;

    memset(has_acted, 0, sizeof(has_acted));
    last_raiser = -1;
}

void server_deal(game_state_t *g){
    for (int i = 0; i < MAX_PLAYERS; i++) if (g->player_status[i] == PLAYER_ACTIVE) {
        g->player_hands[i][0] = g->deck[g->next_card++];
        g->player_hands[i][1] = g->deck[g->next_card++];
    }
}

int check_betting_end(game_state_t *game){
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->player_status[i] == PLAYER_ACTIVE &&
            game->current_bets[i] != game->highest_bet) {
            return 0;
        }
    }
    return 1;
}

int server_bet(game_state_t *g){
    if (check_betting_end(g))
        return 1;
    do {
        g->current_player = (g->current_player + 1) % MAX_PLAYERS;
    } while (g->player_status[g->current_player] != PLAYER_ACTIVE);
    return 0;
}

void server_community(game_state_t *g){
    if (g->round_stage == ROUND_PREFLOP) {
        for (int i = 0; i < 3; i++) g->community_cards[i] = g->deck[g->next_card++];
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

void server_end(game_state_t *g){
    int winner = find_winner(g);
    log_info("Hand ended, winner = %d", winner);
    g->player_stacks[winner] += g->pot_size;
    g->round_stage = ROUND_SHOWDOWN;
}

int evaluate_hand(game_state_t *g, player_id_t p);

int find_winner(game_state_t *game){
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

static int cmp_desc(const void *a, const void *b) {
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    return ib - ia;
}
static int pack_detail(const int *vals, int n) {
    int out = 0;
    for (int i = 0; i < n; ++i) {
        out = (out << 4) | vals[i];
    }
    return out;
}
static int hand_value(card_t *cards, int count) {
    int rank_cnt[13] = {0}, suit_cnt[4] = {0};
    for (int i = 0; i < count; ++i) {
        int r = cards[i] >> SUITE_BITS;
        int s = cards[i] & ((1 << SUITE_BITS) - 1);
        rank_cnt[r]++;
        suit_cnt[s]++;
    }

    // build unique descending ranks (with A-low trick)
    int uniq[14], u = 0;
    for (int r = 12; r >= 0; --r)
        if (rank_cnt[r]) uniq[u++] = r;
    if (rank_cnt[12]) uniq[u++] = -1;  // Ace as low

    // (1) check straight-flush
    int best_sf = -1;
    for (int s = 0; s < 4; ++s) if (suit_cnt[s] >= 5) {
        int sr[7], m = 0;
        for (int i = 0; i < count; ++i)
            if ((cards[i] & ((1 << SUITE_BITS)-1)) == s)
                sr[m++] = cards[i] >> SUITE_BITS;
        qsort(sr, m, sizeof *sr, cmp_desc);
        int uniq_sr[8], k = 0, last = -2;
        for (int i = 0; i < m; ++i)
            if (sr[i] != last) {
                uniq_sr[k++] = sr[i];
                last = sr[i];
            }
        if (last == 12) uniq_sr[k++] = -1;
        for (int i = 0; i + 4 < k; ++i)
            if (uniq_sr[i] - uniq_sr[i+4] == 4) {
                best_sf = uniq_sr[i] < 0 ? 3 : uniq_sr[i];
                break;
            }
        if (best_sf >= 0) break;
    }
    if (best_sf >= 0) return (8<<20)|(best_sf<<16);

    // (2) four-of-a-kind
    for (int r = 12; r >= 0; --r)
        if (rank_cnt[r] == 4) {
            int kc = -1;
            for (int x = 12; x >= 0; --x)
                if (x!=r && rank_cnt[x]) { kc = x; break; }
            return (7<<20)|(r<<16)|(kc<<12);
        }

    // (3) full house
    int three = -1, pair = -1;
    for (int r = 12; r >= 0; --r)
        if (rank_cnt[r] >= 3) { three = r; break; }
    if (three>=0)
        for (int r = 12; r >= 0; --r)
            if (r!=three && rank_cnt[r]>=2) { pair = r; break; }
    if (three>=0 && pair>=0) return (6<<20)|(three<<16)|(pair<<12);

    // (4) flush
    for (int s = 0; s < 4; ++s) if (suit_cnt[s] >= 5) {
        int vals[5], k=0;
        for (int r = 12; r >= 0 && k<5; --r)
            for (int i = 0; i < count && k<5; ++i)
                if (((cards[i]&((1<<SUITE_BITS)-1))==s) &&
                    ((cards[i]>>SUITE_BITS)==r))
                    vals[k++] = r;
        return (5<<20) | pack_detail(vals,5);
    }

    // (5) straight
    for (int i = 0; i+4< u; ++i) {
        int hi = uniq[i]<0?3:uniq[i];
        int lo = uniq[i+4]<0?0:uniq[i+4];
        if (hi - lo == 4) return (4<<20)|(hi<<16);
    }

    // (6) three-of-a-kind
    if (three>=0) {
        int kics[2], k=0;
        for (int r = 12; r>=0 && k<2; --r)
            if (r!=three && rank_cnt[r]) kics[k++]=r;
        return (3<<20)|(three<<16)|pack_detail(kics,2);
    }

    // (7) two-pair
    int p1=-1,p2=-1;
    for (int r = 12; r>=0; --r)
        if (rank_cnt[r]>=2) {
            if (p1<0) p1=r; else if (p2<0) p2=r;
            if (p2>=0) break;
        }
    if (p2>=0) {
        int kic=-1;
        for (int r=12;r>=0;--r)
            if (r!=p1 && r!=p2 && rank_cnt[r]) { kic = r; break; }
        int v[3]={p1,p2,kic};
        return (2<<20)|pack_detail(v,3);
    }

    // (8) one-pair
    if (p1>=0) {
        int kics[3], k=0;
        for (int r=12;r>=0&&k<3;--r)
            if (r!=p1 && rank_cnt[r]) kics[k++]=r;
        int v[4]={p1,kics[0],kics[1],kics[2]};
        return (1<<20)|pack_detail(v,4);
    }

    // (9) high-card
    int hc[5], h=0;
    for (int r=12;r>=0&&h<5;--r) if (rank_cnt[r]) hc[h++]=r;
    while (h<5) hc[h++]=0;
    return pack_detail(hc,5);
}

int evaluate_hand(game_state_t *g, player_id_t p) {
    card_t buf[7]; int n=0;
    if (g->player_hands[p][0]!=NOCARD) buf[n++]=g->player_hands[p][0];
    if (g->player_hands[p][1]!=NOCARD) buf[n++]=g->player_hands[p][1];
    for (int i=0;i<MAX_COMMUNITY_CARDS;++i)
        if (g->community_cards[i]!=NOCARD) buf[n++]=g->community_cards[i];
    return hand_value(buf,n);
}
